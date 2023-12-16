/*
 * Copyright (c) 2013-2014 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>

#include "kore.h"

struct kore_pool		nb_pool;

void
net_init(void)
{
	kore_pool_init(&nb_pool, "nb_pool", sizeof(struct netbuf), 1000);
}

void
net_send_queue(struct connection *c, void *data, u_int32_t len,
    struct spdy_stream *s, int before)
{
	u_int8_t		*d;
	struct netbuf		*nb;
	u_int32_t		avail;

	kore_debug("net_send_queue(%p, %p, %d, %p, %d)",
	    c, data, len, s, before);

	d = data;
	if (before == NETBUF_LAST_CHAIN) {
		nb = TAILQ_LAST(&(c->send_queue), netbuf_head);
		if (nb != NULL && !(nb->flags & NETBUF_IS_STREAM) &&
		    nb->stream == s && nb->b_len < nb->m_len) {
			avail = nb->m_len - nb->b_len;
			if (len < avail) {
				memcpy(nb->buf + nb->b_len, d, len);
				nb->b_len += len;
				return;
			} else if (len > avail) {
				memcpy(nb->buf + nb->b_len, d, avail);
				nb->b_len += avail;

				len -= avail;
				d += avail;
				if (len == 0)
					return;
			}
		}
	}

	nb = kore_pool_get(&nb_pool);
	nb->flags = 0;
	nb->cb = NULL;
	nb->owner = c;
	nb->s_off = 0;
	nb->stream = s;
	nb->b_len = len;
	nb->type = NETBUF_SEND;

	if (nb->b_len < NETBUF_SEND_PAYLOAD_MAX)
		nb->m_len = NETBUF_SEND_PAYLOAD_MAX;
	else
		nb->m_len = nb->b_len;

	nb->buf = kore_malloc(nb->m_len);
	if (len > 0)
		memcpy(nb->buf, d, nb->b_len);

	if (before == NETBUF_BEFORE_CHAIN) {
		TAILQ_INSERT_BEFORE(c->snb, nb, list);
	} else {
		TAILQ_INSERT_TAIL(&(c->send_queue), nb, list);
	}
}

void
net_send_stream(struct connection *c, void *data, u_int32_t len,
    struct spdy_stream *s)
{
	struct netbuf		*nb;

	kore_debug("net_send_stream(%p, %p, %d, %p)", c, data, len, s);

	nb = kore_pool_get(&nb_pool);
	nb->cb = NULL;
	nb->owner = c;
	nb->s_off = 0;
	nb->buf = data;
	nb->stream = s;
	nb->b_len = len;
	nb->m_len = nb->b_len;
	nb->type = NETBUF_SEND;
	nb->flags  = NETBUF_IS_STREAM;

	TAILQ_INSERT_TAIL(&(c->send_queue), nb, list);
}

void
net_recv_queue(struct connection *c, size_t len, int flags,
    struct netbuf **out, int (*cb)(struct netbuf *))
{
	struct netbuf		*nb;

	nb = kore_pool_get(&nb_pool);
	nb->cb = cb;
	nb->b_len = len;
	nb->m_len = len;
	nb->owner = c;
	nb->s_off = 0;
	nb->stream = NULL;
	nb->flags = flags;
	nb->type = NETBUF_RECV;
	nb->buf = kore_malloc(nb->b_len);

	TAILQ_INSERT_TAIL(&(c->recv_queue), nb, list);
	if (out != NULL)
		*out = nb;
}

int
net_recv_expand(struct connection *c, struct netbuf *nb, size_t len,
    int (*cb)(struct netbuf *))
{
	if (nb->type != NETBUF_RECV) {
		kore_debug("net_recv_expand(): wrong netbuf type");
		return (KORE_RESULT_ERROR);
	}

	nb->cb = cb;
	nb->b_len += len;
	nb->m_len = nb->b_len;
	nb->buf = kore_realloc(nb->buf, nb->b_len);

	TAILQ_REMOVE(&(c->recv_queue), nb, list);
	TAILQ_INSERT_HEAD(&(c->recv_queue), nb, list);

	return (KORE_RESULT_OK);
}

int
net_send(struct connection *c)
{
	int			r;
	u_int32_t		len, smin;

	c->snb = TAILQ_FIRST(&(c->send_queue));
	if (c->snb->b_len != 0) {
		if (c->snb->stream != NULL &&
		    (c->snb->stream->flags & SPDY_DATAFRAME_PRELUDE)) {
			if (!spdy_dataframe_begin(c)) {
				c->snb = NULL;
				return (KORE_RESULT_OK);
			}

			c->snb = TAILQ_FIRST(&(c->send_queue));
		}

		smin = c->snb->b_len - c->snb->s_off;
		if (c->snb->stream != NULL &&
		    c->snb->stream->frame_size > 0) {
			smin = MIN(smin, c->snb->stream->frame_size);
		}

		len = MIN(NETBUF_SEND_PAYLOAD_MAX, smin);

#if !defined(KORE_BENCHMARK)
		r = SSL_write(c->ssl,
		    (c->snb->buf + c->snb->s_off), len);
		if (r <= 0) {
			r = SSL_get_error(c->ssl, r);
			switch (r) {
			case SSL_ERROR_WANT_READ:
			case SSL_ERROR_WANT_WRITE:
				c->snb->flags |= NETBUF_MUST_RESEND;
				c->flags &= ~CONN_WRITE_POSSIBLE;
				return (KORE_RESULT_OK);
			default:
				kore_debug("SSL_write(): %s",
				    ssl_errno_s);
				return (KORE_RESULT_ERROR);
			}
		}
#else
		r = write(c->fd, (c->snb->buf + c->snb->s_off), len);
		if (r <= -1) {
			switch (errno) {
			case EINTR:
			case EAGAIN:
				c->flags &= ~CONN_WRITE_POSSIBLE;
				return (KORE_RESULT_OK);
			default:
				kore_debug("write: %s", errno_s);
				return (KORE_RESULT_ERROR);
			}
		}
#endif
		kore_debug("net_send(%p/%d/%d bytes), progress with %d",
		    c->snb, c->snb->s_off, c->snb->b_len, r);

		c->snb->s_off += (size_t)r;
		c->snb->flags &= ~NETBUF_MUST_RESEND;
		if (c->snb->stream != NULL)
			spdy_update_wsize(c, c->snb->stream, r);
	}

	if (c->snb->s_off == c->snb->b_len ||
	    (c->snb->flags & NETBUF_FORCE_REMOVE)) {
		net_remove_netbuf(&(c->send_queue), c->snb);
		c->snb = NULL;
	}

	return (KORE_RESULT_OK);
}

int
net_send_flush(struct connection *c)
{
	kore_debug("net_send_flush(%p)", c);

	while (!TAILQ_EMPTY(&(c->send_queue)) &&
	    (c->flags & CONN_WRITE_POSSIBLE)) {
		if (!net_send(c))
			return (KORE_RESULT_ERROR);
	}

	if ((c->flags & CONN_CLOSE_EMPTY) && TAILQ_EMPTY(&(c->send_queue)))
		kore_connection_disconnect(c);

	return (KORE_RESULT_OK);
}

int
net_recv(struct connection *c)
{
	int			r;

	c->rnb = TAILQ_FIRST(&(c->recv_queue));
	if (c->rnb == NULL) {
		kore_debug("kore_read_client(): nb->cb == NULL");
		return (KORE_RESULT_ERROR);
	}

#if !defined(KORE_BENCHMARK)
	r = SSL_read(c->ssl,
	    (c->rnb->buf + c->rnb->s_off),
	    (c->rnb->b_len - c->rnb->s_off));
	if (r <= 0) {
		r = SSL_get_error(c->ssl, r);
		switch (r) {
		case SSL_ERROR_WANT_READ:
		case SSL_ERROR_WANT_WRITE:
			c->flags &= ~CONN_READ_POSSIBLE;
			return (KORE_RESULT_OK);
		default:
			kore_debug("SSL_read(): %s", ssl_errno_s);
			return (KORE_RESULT_ERROR);
		}
	}
#else
	r = read(c->fd, (c->rnb->buf + c->rnb->s_off),
	    (c->rnb->b_len - c->rnb->s_off));
	if (r <= 0) {
		switch (errno) {
		case EINTR:
		case EAGAIN:
			c->flags &= ~CONN_READ_POSSIBLE;
			return (KORE_RESULT_OK);
		default:
			kore_debug("read(): %s", errno_s);
			return (KORE_RESULT_ERROR);
		}
	}
#endif
	kore_debug("net_recv(%ld/%ld bytes), progress with %d",
	    c->rnb->s_off, c->rnb->b_len, r);

	c->rnb->s_off += (size_t)r;
	if (c->rnb->s_off == c->rnb->b_len ||
	    (c->rnb->flags & NETBUF_CALL_CB_ALWAYS)) {
		r = c->rnb->cb(c->rnb);
		if (c->rnb->s_off == c->rnb->b_len ||
		    (c->rnb->flags & NETBUF_FORCE_REMOVE)) {
			net_remove_netbuf(&(c->recv_queue), c->rnb);
			c->rnb = NULL;
		}

		if (r != KORE_RESULT_OK)
			return (r);
	}

	return (KORE_RESULT_OK);
}

int
net_recv_flush(struct connection *c)
{
	kore_debug("net_recv_flush(%p)", c);

	while (!TAILQ_EMPTY(&(c->recv_queue)) &&
	    (c->flags & CONN_READ_POSSIBLE)) {
		if (!net_recv(c))
			return (KORE_RESULT_ERROR);
	}

	return (KORE_RESULT_OK);
}

void
net_remove_netbuf(struct netbuf_head *list, struct netbuf *nb)
{
	kore_debug("net_remove_netbuf(%p, %p, %p)", list, nb, nb->stream);

	nb->stream = NULL;
	if (nb->flags & NETBUF_MUST_RESEND) {
		kore_debug("retaining %p (MUST_RESEND)", nb);
		nb->flags |= NETBUF_FORCE_REMOVE;
		return;
	}

	if (!(nb->flags & NETBUF_IS_STREAM))
		kore_mem_free(nb->buf);

	TAILQ_REMOVE(list, nb, list);
	kore_pool_put(&nb_pool, nb);
}

u_int16_t
net_read16(u_int8_t *b)
{
	u_int16_t	r;

	r = *(u_int16_t *)b;
	return (ntohs(r));
}

u_int32_t
net_read32(u_int8_t *b)
{
	u_int32_t	r;

	r = *(u_int32_t *)b;
	return (ntohl(r));
}

void
net_write16(u_int8_t *p, u_int16_t n)
{
	u_int16_t	r;

	r = htons(n);
	memcpy(p, &r, sizeof(r));
}

void
net_write32(u_int8_t *p, u_int32_t n)
{
	u_int32_t	r;

	r = htonl(n);
	memcpy(p, &r, sizeof(r));
}
