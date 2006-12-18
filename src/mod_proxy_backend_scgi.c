#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#include "inet_ntop_cache.h"
#include "mod_proxy_core.h"
#include "mod_proxy_core_protocol.h"
#include "buffer.h"
#include "log.h"
#include "array.h"

#define CORE_PLUGIN "mod_proxy_core"

typedef struct {
	PLUGIN_DATA;

	proxy_protocol *protocol;
} protocol_plugin_data;

/*
SESSION_FUNC(proxy_scgi_init) {
	return 1;
}
*/

/*
SESSION_FUNC(proxy_scgi_cleanup) {
	return 1;
}
*/

/**
 * add a key-value pair to the scgi-buffer
 */
static int scgi_env_add(buffer *env, const char *key, size_t key_len, const char *val, size_t val_len) {
	size_t len;

	if (!key || !val) return -1;

	len = key_len + val_len + 2;

	buffer_prepare_append(env, len);

	/* include the NUL */
	buffer_append_memory(env, key, key_len + 1);
	buffer_append_memory(env, val, val_len + 1);

	return 0;
}

static int proxy_scgi_get_env_scgi(server *srv, proxy_session *sess, buffer *env_headers) {
	connection *con = sess->remote_con;
	server_socket *srv_sock = con->srv_socket;
	plugin_data *p = sess->p;
	socklen_t our_addr_len;
	sock_addr our_addr;
	const char *s;
	char buf[32];
	int len;
#ifdef HAVE_IPV6
	char b2[INET6_ADDRSTRLEN + 1];
#endif

	/* CGI-SPEC 6.1.2 and FastCGI spec 6.3 */

	/* request.content_length < SSIZE_MAX, see request.c */
	if(con->request.content_length > 0) {
		len = ltostr(buf, con->request.content_length);
	} else {
		buf[0] = '0';
		buf[1] = '\0';
		len = 1;
	}
	scgi_env_add(env_headers, CONST_STR_LEN("CONTENT_LENGTH"), buf, len);
	scgi_env_add(env_headers, CONST_STR_LEN("SCGI"), CONST_STR_LEN("1"));

	scgi_env_add(env_headers, CONST_STR_LEN("SERVER_SOFTWARE"), CONST_STR_LEN(PACKAGE_NAME"/"PACKAGE_VERSION));

	if (con->server_name->used) {
		scgi_env_add(env_headers, CONST_STR_LEN("SERVER_NAME"), CONST_BUF_LEN(con->server_name));
	} else {
#ifdef HAVE_IPV6
		s = inet_ntop(srv_sock->addr.plain.sa_family,
			      srv_sock->addr.plain.sa_family == AF_INET6 ?
			      (const void *) &(srv_sock->addr.ipv6.sin6_addr) :
			      (const void *) &(srv_sock->addr.ipv4.sin_addr),
			      b2, sizeof(b2)-1);
#else
		s = inet_ntoa(srv_sock->addr.ipv4.sin_addr);
#endif
		scgi_env_add(env_headers, CONST_STR_LEN("SERVER_NAME"), s, strlen(s));
	}

	scgi_env_add(env_headers, CONST_STR_LEN("GATEWAY_INTERFACE"), CONST_STR_LEN("CGI/1.1"));

	len = ltostr(buf,
#ifdef HAVE_IPV6
	       ntohs(srv_sock->addr.plain.sa_family ? srv_sock->addr.ipv6.sin6_port : srv_sock->addr.ipv4.sin_port)
#else
	       ntohs(srv_sock->addr.ipv4.sin_port)
#endif
	       );

	scgi_env_add(env_headers, CONST_STR_LEN("SERVER_PORT"), buf, len);

	/* get the server-side of the connection to the client */
	our_addr_len = sizeof(our_addr);

	if (-1 == getsockname(con->sock->fd, &(our_addr.plain), &our_addr_len)) {
		s = inet_ntop_cache_get_ip(srv, &(srv_sock->addr));
	} else {
		s = inet_ntop_cache_get_ip(srv, &(our_addr));
	}
	scgi_env_add(env_headers, CONST_STR_LEN("SERVER_ADDR"), s, strlen(s));

	len = ltostr(buf,
#ifdef HAVE_IPV6
	       ntohs(con->dst_addr.plain.sa_family ? con->dst_addr.ipv6.sin6_port : con->dst_addr.ipv4.sin_port)
#else
	       ntohs(con->dst_addr.ipv4.sin_port)
#endif
	       );

	scgi_env_add(env_headers, CONST_STR_LEN("REMOTE_PORT"), buf, len);

	s = inet_ntop_cache_get_ip(srv, &(con->dst_addr));
	scgi_env_add(env_headers, CONST_STR_LEN("REMOTE_ADDR"), s, strlen(s));

	if (!buffer_is_empty(con->authed_user)) {
		scgi_env_add(env_headers, CONST_STR_LEN("REMOTE_USER"),
			     CONST_BUF_LEN(con->authed_user));
	}

	/*
	 * SCRIPT_NAME, PATH_INFO and PATH_TRANSLATED according to
	 * http://cgi-spec.golux.com/draft-coar-cgi-v11-03-clean.html
	 * (6.1.14, 6.1.6, 6.1.7)
	 * For AUTHORIZER mode these headers should be omitted.
	 */

	scgi_env_add(env_headers, CONST_STR_LEN("SCRIPT_NAME"), CONST_BUF_LEN(con->uri.path));

	if (!buffer_is_empty(con->request.pathinfo)) {
		scgi_env_add(env_headers, CONST_STR_LEN("PATH_INFO"), CONST_BUF_LEN(con->request.pathinfo));

		/* PATH_TRANSLATED is only defined if PATH_INFO is set */

		buffer_copy_string_buffer(p->tmp_buf, con->physical.doc_root);
		buffer_append_string_buffer(p->tmp_buf, con->request.pathinfo);
		scgi_env_add(env_headers, CONST_STR_LEN("PATH_TRANSLATED"), CONST_BUF_LEN(p->tmp_buf));
	} else {
		scgi_env_add(env_headers, CONST_STR_LEN("PATH_INFO"), CONST_STR_LEN(""));
	}

	/*
	 * SCRIPT_FILENAME and DOCUMENT_ROOT for php. The PHP manual
	 * http://www.php.net/manual/en/reserved.variables.php
	 * treatment of PATH_TRANSLATED is different from the one of CGI specs.
	 * TODO: this code should be checked against cgi.fix_pathinfo php
	 * parameter.
	 */

	if (1) {
		scgi_env_add(env_headers, CONST_STR_LEN("SCRIPT_FILENAME"), CONST_BUF_LEN(con->physical.path));
		scgi_env_add(env_headers, CONST_STR_LEN("DOCUMENT_ROOT"), CONST_BUF_LEN(con->physical.doc_root));
	}

	scgi_env_add(env_headers, CONST_STR_LEN("REQUEST_URI"), CONST_BUF_LEN(con->request.orig_uri));

	if (!buffer_is_equal(sess->request_uri, con->request.orig_uri)) {
		scgi_env_add(env_headers, CONST_STR_LEN("REDIRECT_URI"), CONST_BUF_LEN(sess->request_uri));
	}
	if (!buffer_is_empty(con->uri.query)) {
		scgi_env_add(env_headers, CONST_STR_LEN("QUERY_STRING"), CONST_BUF_LEN(con->uri.query));
	} else {
		scgi_env_add(env_headers, CONST_STR_LEN("QUERY_STRING"), CONST_STR_LEN(""));
	}

	s = get_http_method_name(con->request.http_method);
	scgi_env_add(env_headers, CONST_STR_LEN("REQUEST_METHOD"), s, strlen(s));
	scgi_env_add(env_headers, CONST_STR_LEN("REDIRECT_STATUS"), CONST_STR_LEN("200")); /* if php is compiled with --force-redirect */
	s = get_http_version_name(con->request.http_version);
	scgi_env_add(env_headers, CONST_STR_LEN("SERVER_PROTOCOL"), s, strlen(s));

#ifdef USE_OPENSSL
	if (srv_sock->is_ssl) {
		scgi_env_add(env_headers, CONST_STR_LEN("HTTPS"), CONST_STR_LEN("on"));
	}
#endif

	return 0;
}

/**
 * transform the HTTP-Request headers into CGI notation
 */
static int proxy_scgi_get_env_request(server *srv, proxy_session *sess, buffer *env_headers) {
	connection *con = sess->remote_con;
	plugin_data *p = sess->p;
	size_t i;

	UNUSED(srv);

	/* the request header got already copied into the sess->request_headers for us
	 * no extra filter is needed
	 *
	 * prepend a HTTP_ and uppercase the keys
	 */
	for (i = 0; i < sess->request_headers->used; i++) {
		data_string *ds;
		size_t j;

		ds = (data_string *)sess->request_headers->data[i];
		if (ds->value->used == 0 || ds->key->used == 0) continue;

		buffer_reset(p->tmp_buf);

		if (0 != strcasecmp(ds->key->ptr, "CONTENT-TYPE")) {
			BUFFER_COPY_STRING_CONST(p->tmp_buf, "HTTP_");
			p->tmp_buf->used--;
		}

		buffer_prepare_append(p->tmp_buf, ds->key->used + 2);
		for (j = 0; j < ds->key->used - 1; j++) {
			char c = ds->key->ptr[j];
			if (light_isalpha(c)) {
				/* upper-case */
				c = toupper(c);
			} else if (light_isdigit(c)) {
				/* copy */
				c = c;
			} else {
				c = '_';
			}
			p->tmp_buf->ptr[p->tmp_buf->used++] = c;
		}
		p->tmp_buf->ptr[p->tmp_buf->used++] = '\0';

		scgi_env_add(env_headers, CONST_BUF_LEN(p->tmp_buf), CONST_BUF_LEN(ds->value));
	}

	for (i = 0; i < con->environment->used; i++) {
		data_string *ds;
		size_t j;

		ds = (data_string *)con->environment->data[i];
		if (ds->value->used == 0 || ds->key->used == 0) continue;

		buffer_reset(p->tmp_buf);

		buffer_prepare_append(p->tmp_buf, ds->key->used + 2);
		for (j = 0; j < ds->key->used - 1; j++) {
			char c = ds->key->ptr[j];
			if (light_isalpha(c)) {
				/* upper-case */
				c = toupper(c);
			} else {
				c = '_';
			}
			p->tmp_buf->ptr[p->tmp_buf->used++] = c;
		}
		p->tmp_buf->ptr[p->tmp_buf->used++] = '\0';

		scgi_env_add(env_headers, CONST_BUF_LEN(p->tmp_buf), CONST_BUF_LEN(ds->value));
	}

	return 0;
}

STREAM_IN_OUT_FUNC(proxy_scgi_get_request_chunk) {
	size_t headers_len = 0;
	buffer *len_buf;
	buffer *env_headers;

	UNUSED(in);

	/* get buffer to write headers length into */
	len_buf = chunkqueue_get_append_buffer(out);

	/* write SCGI request headers */
	env_headers = chunkqueue_get_append_buffer(out);
	buffer_prepare_copy(env_headers, 1024);
	proxy_scgi_get_env_scgi(srv, sess, env_headers);
	proxy_scgi_get_env_request(srv, sess, env_headers);
	headers_len = env_headers->used;
	out->bytes_in += headers_len;

	/* append "," after headers */
	buffer_append_memory(env_headers, CONST_STR_LEN(","));
	env_headers->used++; /* this is needed because the network will only write "used - 1" bytes */
	out->bytes_in++;

	/* prepend [headers_len]: */
	buffer_append_long(len_buf, headers_len);
	buffer_append_string_len(len_buf, CONST_STR_LEN(":"));
	out->bytes_in += len_buf->used - 1;

	return 0;
}

STREAM_IN_OUT_FUNC(proxy_scgi_stream_decoder) {
	chunk *c;

	UNUSED(srv);

	if (in->first == NULL) {
		if (in->is_closed) return 1;

		return 0;
	}

	/* no chunked encoding, ok, perhaps a content-length ? */

	chunkqueue_remove_finished_chunks(in);
	for (c = in->first; c; c = c->next) {
		buffer *b;

		if (c->mem->used == 0) continue;

		out->bytes_in += c->mem->used - c->offset - 1;
		in->bytes_out += c->mem->used - c->offset - 1;

		sess->bytes_read += c->mem->used - c->offset - 1;

		if (c->offset == 0) {
			/* we are copying the whole buffer, just steal it */

			chunkqueue_steal_chunk(out, c);
		} else {
			b = chunkqueue_get_append_buffer(out);
			buffer_copy_string_len(b, c->mem->ptr + c->offset, c->mem->used - c->offset - 1);
			c->offset = c->mem->used - 1; /* marks is read */
		}

		if (sess->bytes_read == sess->content_length) {
			break;
		}

	}

	if (in->is_closed || sess->bytes_read == sess->content_length) {
		return 1; /* finished */
	}

	return 0;
}

/**
 * transform the content-stream into a valid HTTP-content-stream
 *
 * as we don't apply chunked-encoding here, pass it on AS IS
 */
STREAM_IN_OUT_FUNC(proxy_scgi_stream_encoder) {
	chunk *c;

	UNUSED(srv);
	UNUSED(sess);

	/* there is nothing that we have to send out anymore */
	if (in->bytes_in == in->bytes_out && 
	    in->is_closed) return 0;

	for (c = in->first; in->bytes_out < in->bytes_in; c = c->next) {
		buffer *b;
		off_t weWant = in->bytes_in - in->bytes_out;
		off_t weHave = 0;

		/* we announce toWrite octects
		 * now take all the request_content chunk that we need to fill this request
		 */

		switch (c->type) {
		case FILE_CHUNK:
			weHave = c->file.length - c->offset;

			if (weHave > weWant) weHave = weWant;

			/** steal the chunk from the incoming chunkqueue */	
			chunkqueue_steal_tempfile(out, c);

			c->offset += weHave;
			in->bytes_out += weHave;

			out->bytes_in += weHave;

			break;
		case MEM_CHUNK:
			/* append to the buffer */
			weHave = c->mem->used - 1 - c->offset;

			if (weHave > weWant) weHave = weWant;

			b = chunkqueue_get_append_buffer(out);
			buffer_append_memory(b, c->mem->ptr + c->offset, weHave);
			b->used++; /* add virtual \0 */

			c->offset += weHave;
			in->bytes_out += weHave;

			out->bytes_in += weHave;

			break;
		default:
			break;
		}
	}

	return 0;

}

/**
 * parse the response header
 *
 * - scgi needs some decoding for the protocol
 */
STREAM_IN_OUT_FUNC(proxy_scgi_parse_response_header) {
	UNUSED(srv);
	UNUSED(out);

	http_response_reset(sess->resp);

	/* backend response already in HTTP response format, no special parsing needed. */
	return http_response_parse_cq(in, sess->resp);
}

INIT_FUNC(mod_proxy_backend_scgi_init) {
	mod_proxy_core_plugin_data *core_data;
	protocol_plugin_data *p;

	/* get the plugin_data of the core-plugin */
	core_data = plugin_get_config(srv, CORE_PLUGIN);
	if(!core_data) return NULL;

	p = calloc(1, sizeof(*p));

	/* define protocol handler callbacks */
	p->protocol = core_data->proxy_register_protocol("scgi");

	/*
	p->protocol->proxy_stream_init = proxy_scgi_init;
	p->protocol->proxy_stream_cleanup = proxy_scgi_cleanup;
	*/
	p->protocol->proxy_stream_decoder = proxy_scgi_stream_decoder;
	p->protocol->proxy_stream_encoder = proxy_scgi_stream_encoder;
	p->protocol->proxy_get_request_chunk = proxy_scgi_get_request_chunk;
	p->protocol->proxy_parse_response_header = proxy_scgi_parse_response_header;

	return p;
}

int mod_proxy_backend_scgi_plugin_init(plugin *p) {
	data_string *ds;

	p->version      = LIGHTTPD_VERSION_ID;
	p->name         = buffer_init_string("mod_proxy_backend_scgi");

	p->init         = mod_proxy_backend_scgi_init;

	p->data         = NULL;

	ds = data_string_init();
	buffer_copy_string(ds->value, CORE_PLUGIN);
	array_insert_unique(p->required_plugins, (data_unset *)ds);

	return 0;
}
