/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>

#include <microhttpd.h>

#include "log.h"
#include "util.h"
#include "sd-journal.h"
#include "sd-daemon.h"
#include "logs-show.h"
#include "microhttpd-util.h"
#include "virt.h"
#include "build.h"

typedef struct RequestMeta {
        sd_journal *journal;

        OutputMode mode;

        char *cursor;
        int64_t n_skip;
        uint64_t n_entries;
        bool n_entries_set;

        FILE *tmp;
        uint64_t delta, size;

        int argument_parse_error;

        bool follow;
        bool discrete;

        uint64_t n_fields;
        bool n_fields_set;
} RequestMeta;

static const char* const mime_types[_OUTPUT_MODE_MAX] = {
        [OUTPUT_SHORT] = "text/plain",
        [OUTPUT_JSON] = "application/json",
        [OUTPUT_JSON_SSE] = "text/event-stream",
        [OUTPUT_EXPORT] = "application/vnd.fdo.journal",
};

static RequestMeta *request_meta(void **connection_cls) {
        RequestMeta *m;

        if (*connection_cls)
                return *connection_cls;

        m = new0(RequestMeta, 1);
        if (!m)
                return NULL;

        *connection_cls = m;
        return m;
}

static void request_meta_free(
                void *cls,
                struct MHD_Connection *connection,
                void **connection_cls,
                enum MHD_RequestTerminationCode toe) {

        RequestMeta *m = *connection_cls;

        if (!m)
                return;

        if (m->journal)
                sd_journal_close(m->journal);

        if (m->tmp)
                fclose(m->tmp);

        free(m->cursor);
        free(m);
}

static int open_journal(RequestMeta *m) {
        assert(m);

        if (m->journal)
                return 0;

        return sd_journal_open(&m->journal, SD_JOURNAL_LOCAL_ONLY|SD_JOURNAL_SYSTEM_ONLY);
}


static int respond_oom_internal(struct MHD_Connection *connection) {
        struct MHD_Response *response;
        const char m[] = "Out of memory.\n";
        int ret;

        assert(connection);

        response = MHD_create_response_from_buffer(sizeof(m)-1, (char*) m, MHD_RESPMEM_PERSISTENT);
        if (!response)
                return MHD_NO;

        MHD_add_response_header(response, "Content-Type", "text/plain");
        ret = MHD_queue_response(connection, MHD_HTTP_SERVICE_UNAVAILABLE, response);
        MHD_destroy_response(response);

        return ret;
}

#define respond_oom(connection) log_oom(), respond_oom_internal(connection)

static int respond_error(
                struct MHD_Connection *connection,
                unsigned code,
                const char *format, ...) {

        struct MHD_Response *response;
        char *m;
        int r;
        va_list ap;

        assert(connection);
        assert(format);

        va_start(ap, format);
        r = vasprintf(&m, format, ap);
        va_end(ap);

        if (r < 0)
                return respond_oom(connection);

        response = MHD_create_response_from_buffer(strlen(m), m, MHD_RESPMEM_MUST_FREE);
        if (!response) {
                free(m);
                return respond_oom(connection);
        }

        MHD_add_response_header(response, "Content-Type", "text/plain");
        r = MHD_queue_response(connection, code, response);
        MHD_destroy_response(response);

        return r;
}

static ssize_t request_reader_entries(
                void *cls,
                uint64_t pos,
                char *buf,
                size_t max) {

        RequestMeta *m = cls;
        int r;
        size_t n, k;

        assert(m);
        assert(buf);
        assert(max > 0);
        assert(pos >= m->delta);

        pos -= m->delta;

        while (pos >= m->size) {
                off_t sz;

                /* End of this entry, so let's serialize the next
                 * one */

                if (m->n_entries_set &&
                    m->n_entries <= 0)
                        return MHD_CONTENT_READER_END_OF_STREAM;

                if (m->n_skip < 0)
                        r = sd_journal_previous_skip(m->journal, (uint64_t) -m->n_skip + 1);
                else if (m->n_skip > 0)
                        r = sd_journal_next_skip(m->journal, (uint64_t) m->n_skip + 1);
                else
                        r = sd_journal_next(m->journal);

                if (r < 0) {
                        log_error("Failed to advance journal pointer: %s", strerror(-r));
                        return MHD_CONTENT_READER_END_WITH_ERROR;
                } else if (r == 0) {

                        if (m->follow) {
                                r = sd_journal_wait(m->journal, (uint64_t) -1);
                                if (r < 0) {
                                        log_error("Couldn't wait for journal event: %s", strerror(-r));
                                        return MHD_CONTENT_READER_END_WITH_ERROR;
                                }

                                continue;
                        }

                        return MHD_CONTENT_READER_END_OF_STREAM;
                }

                if (m->discrete) {
                        assert(m->cursor);

                        r = sd_journal_test_cursor(m->journal, m->cursor);
                        if (r < 0) {
                                log_error("Failed to test cursor: %s", strerror(-r));
                                return MHD_CONTENT_READER_END_WITH_ERROR;
                        }

                        if (r == 0)
                                return MHD_CONTENT_READER_END_OF_STREAM;
                }

                pos -= m->size;
                m->delta += m->size;

                if (m->n_entries_set)
                        m->n_entries -= 1;

                m->n_skip = 0;

                if (m->tmp)
                        rewind(m->tmp);
                else {
                        m->tmp = tmpfile();
                        if (!m->tmp) {
                                log_error("Failed to create temporary file: %m");
                                return MHD_CONTENT_READER_END_WITH_ERROR;
                        }
                }

                r = output_journal(m->tmp, m->journal, m->mode, 0, OUTPUT_FULL_WIDTH);
                if (r < 0) {
                        log_error("Failed to serialize item: %s", strerror(-r));
                        return MHD_CONTENT_READER_END_WITH_ERROR;
                }

                sz = ftello(m->tmp);
                if (sz == (off_t) -1) {
                        log_error("Failed to retrieve file position: %m");
                        return MHD_CONTENT_READER_END_WITH_ERROR;
                }

                m->size = (uint64_t) sz;
        }

        if (fseeko(m->tmp, pos, SEEK_SET) < 0) {
                log_error("Failed to seek to position: %m");
                return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        n = m->size - pos;
        if (n > max)
                n = max;

        errno = 0;
        k = fread(buf, 1, n, m->tmp);
        if (k != n) {
                log_error("Failed to read from file: %s", errno ? strerror(errno) : "Premature EOF");
                return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        return (ssize_t) k;
}

static int request_parse_accept(
                RequestMeta *m,
                struct MHD_Connection *connection) {

        const char *header;

        assert(m);
        assert(connection);

        header = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Accept");
        if (!header)
                return 0;

        if (streq(header, mime_types[OUTPUT_JSON]))
                m->mode = OUTPUT_JSON;
        else if (streq(header, mime_types[OUTPUT_JSON_SSE]))
                m->mode = OUTPUT_JSON_SSE;
        else if (streq(header, mime_types[OUTPUT_EXPORT]))
                m->mode = OUTPUT_EXPORT;
        else
                m->mode = OUTPUT_SHORT;

        return 0;
}

static int request_parse_range(
                RequestMeta *m,
                struct MHD_Connection *connection) {

        const char *range, *colon, *colon2;
        int r;

        assert(m);
        assert(connection);

        range = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Range");
        if (!range)
                return 0;

        if (!startswith(range, "entries="))
                return 0;

        range += 8;
        range += strspn(range, WHITESPACE);

        colon = strchr(range, ':');
        if (!colon)
                m->cursor = strdup(range);
        else {
                const char *p;

                colon2 = strchr(colon + 1, ':');
                if (colon2) {
                        char _cleanup_free_ *t;

                        t = strndup(colon + 1, colon2 - colon - 1);
                        if (!t)
                                return -ENOMEM;

                        r = safe_atoi64(t, &m->n_skip);
                        if (r < 0)
                                return r;
                }

                p = (colon2 ? colon2 : colon) + 1;
                if (*p) {
                        r = safe_atou64(p, &m->n_entries);
                        if (r < 0)
                                return r;

                        if (m->n_entries <= 0)
                                return -EINVAL;

                        m->n_entries_set = true;
                }

                m->cursor = strndup(range, colon - range);
        }

        if (!m->cursor)
                return -ENOMEM;

        m->cursor[strcspn(m->cursor, WHITESPACE)] = 0;
        if (isempty(m->cursor)) {
                free(m->cursor);
                m->cursor = NULL;
        }

        return 0;
}

static int request_parse_arguments_iterator(
                void *cls,
                enum MHD_ValueKind kind,
                const char *key,
                const char *value) {

        RequestMeta *m = cls;
        _cleanup_free_ char *p = NULL;
        int r;

        assert(m);

        if (isempty(key)) {
                m->argument_parse_error = -EINVAL;
                return MHD_NO;
        }

        if (streq(key, "follow")) {
                if (isempty(value)) {
                        m->follow = true;
                        return MHD_YES;
                }

                r = parse_boolean(value);
                if (r < 0) {
                        m->argument_parse_error = r;
                        return MHD_NO;
                }

                m->follow = r;
                return MHD_YES;
        }

        if (streq(key, "discrete")) {
                if (isempty(value)) {
                        m->discrete = true;
                        return MHD_YES;
                }

                r = parse_boolean(value);
                if (r < 0) {
                        m->argument_parse_error = r;
                        return MHD_NO;
                }

                m->discrete = r;
                return MHD_YES;
        }

        if (streq(key, "boot")) {
                if (isempty(value))
                        r = true;
                else {
                        r = parse_boolean(value);
                        if (r < 0) {
                                m->argument_parse_error = r;
                                return MHD_NO;
                        }
                }

                if (r) {
                        char match[9 + 32 + 1] = "_BOOT_ID=";
                        sd_id128_t bid;

                        r = sd_id128_get_boot(&bid);
                        if (r < 0) {
                                log_error("Failed to get boot ID: %s", strerror(-r));
                                return MHD_NO;
                        }

                        sd_id128_to_string(bid, match + 9);
                        r = sd_journal_add_match(m->journal, match, sizeof(match)-1);
                        if (r < 0) {
                                m->argument_parse_error = r;
                                return MHD_NO;
                        }
                }

                return MHD_YES;
        }

        p = strjoin(key, "=", strempty(value), NULL);
        if (!p) {
                m->argument_parse_error = log_oom();
                return MHD_NO;
        }

        r = sd_journal_add_match(m->journal, p, 0);
        if (r < 0) {
                m->argument_parse_error = r;
                return MHD_NO;
        }

        return MHD_YES;
}

static int request_parse_arguments(
                RequestMeta *m,
                struct MHD_Connection *connection) {

        assert(m);
        assert(connection);

        m->argument_parse_error = 0;
        MHD_get_connection_values(connection, MHD_GET_ARGUMENT_KIND, request_parse_arguments_iterator, m);

        return m->argument_parse_error;
}

static int request_handler_entries(
                struct MHD_Connection *connection,
                void *connection_cls) {

        struct MHD_Response *response;
        RequestMeta *m = connection_cls;
        int r;

        assert(connection);
        assert(m);

        r = open_journal(m);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to open journal: %s\n", strerror(-r));

        if (request_parse_accept(m, connection) < 0)
                return respond_error(connection, MHD_HTTP_BAD_REQUEST, "Failed to parse Accept header.\n");

        if (request_parse_range(m, connection) < 0)
                return respond_error(connection, MHD_HTTP_BAD_REQUEST, "Failed to parse Range header.\n");

        if (request_parse_arguments(m, connection) < 0)
                return respond_error(connection, MHD_HTTP_BAD_REQUEST, "Failed to parse URL arguments.\n");

        if (m->discrete) {
                if (!m->cursor)
                        return respond_error(connection, MHD_HTTP_BAD_REQUEST, "Discrete seeks require a cursor specification.\n");

                m->n_entries = 1;
                m->n_entries_set = true;
        }

        if (m->cursor)
                r = sd_journal_seek_cursor(m->journal, m->cursor);
        else if (m->n_skip >= 0)
                r = sd_journal_seek_head(m->journal);
        else if (m->n_skip < 0)
                r = sd_journal_seek_tail(m->journal);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_BAD_REQUEST, "Failed to seek in journal.\n");

        response = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 4*1024, request_reader_entries, m, NULL);
        if (!response)
                return respond_oom(connection);

        MHD_add_response_header(response, "Content-Type", mime_types[m->mode]);

        r = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);

        return r;
}

static int output_field(FILE *f, OutputMode m, const char *d, size_t l) {
        const char *eq;
        size_t j;

        eq = memchr(d, '=', l);
        if (!eq)
                return -EINVAL;

        j = l - (eq - d + 1);

        if (m == OUTPUT_JSON) {
                fprintf(f, "{ \"%.*s\" : ", (int) (eq - d), d);
                json_escape(f, eq+1, j, OUTPUT_FULL_WIDTH);
                fputs(" }\n", f);
        } else {
                fwrite(eq+1, 1, j, f);
                fputc('\n', f);
        }

        return 0;
}

static ssize_t request_reader_fields(
                void *cls,
                uint64_t pos,
                char *buf,
                size_t max) {

        RequestMeta *m = cls;
        int r;
        size_t n, k;

        assert(m);
        assert(buf);
        assert(max > 0);
        assert(pos >= m->delta);

        pos -= m->delta;

        while (pos >= m->size) {
                off_t sz;
                const void *d;
                size_t l;

                /* End of this field, so let's serialize the next
                 * one */

                if (m->n_fields_set &&
                    m->n_fields <= 0)
                        return MHD_CONTENT_READER_END_OF_STREAM;

                r = sd_journal_enumerate_unique(m->journal, &d, &l);
                if (r < 0) {
                        log_error("Failed to advance field index: %s", strerror(-r));
                        return MHD_CONTENT_READER_END_WITH_ERROR;
                } else if (r == 0)
                        return MHD_CONTENT_READER_END_OF_STREAM;

                pos -= m->size;
                m->delta += m->size;

                if (m->n_fields_set)
                        m->n_fields -= 1;

                if (m->tmp)
                        rewind(m->tmp);
                else {
                        m->tmp = tmpfile();
                        if (!m->tmp) {
                                log_error("Failed to create temporary file: %m");
                                return MHD_CONTENT_READER_END_WITH_ERROR;
                        }
                }

                r = output_field(m->tmp, m->mode, d, l);
                if (r < 0) {
                        log_error("Failed to serialize item: %s", strerror(-r));
                        return MHD_CONTENT_READER_END_WITH_ERROR;
                }

                sz = ftello(m->tmp);
                if (sz == (off_t) -1) {
                        log_error("Failed to retrieve file position: %m");
                        return MHD_CONTENT_READER_END_WITH_ERROR;
                }

                m->size = (uint64_t) sz;
        }

        if (fseeko(m->tmp, pos, SEEK_SET) < 0) {
                log_error("Failed to seek to position: %m");
                return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        n = m->size - pos;
        if (n > max)
                n = max;

        errno = 0;
        k = fread(buf, 1, n, m->tmp);
        if (k != n) {
                log_error("Failed to read from file: %s", errno ? strerror(errno) : "Premature EOF");
                return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        return (ssize_t) k;
}

static int request_handler_fields(
                struct MHD_Connection *connection,
                const char *field,
                void *connection_cls) {

        struct MHD_Response *response;
        RequestMeta *m = connection_cls;
        int r;

        assert(connection);
        assert(m);

        r = open_journal(m);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to open journal: %s\n", strerror(-r));

        if (request_parse_accept(m, connection) < 0)
                return respond_error(connection, MHD_HTTP_BAD_REQUEST, "Failed to parse Accept header.\n");

        r = sd_journal_query_unique(m->journal, field);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_BAD_REQUEST, "Failed to query unique fields.\n");

        response = MHD_create_response_from_callback(MHD_SIZE_UNKNOWN, 4*1024, request_reader_fields, m, NULL);
        if (!response)
                return respond_oom(connection);

        MHD_add_response_header(response, "Content-Type", mime_types[m->mode == OUTPUT_JSON ? OUTPUT_JSON : OUTPUT_SHORT]);

        r = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);

        return r;
}

static int request_handler_redirect(
                struct MHD_Connection *connection,
                const char *target) {

        char *page;
        struct MHD_Response *response;
        int ret;

        assert(connection);
        assert(target);

        if (asprintf(&page, "<html><body>Please continue to the <a href=\"%s\">journal browser</a>.</body></html>", target) < 0)
                return respond_oom(connection);

        response = MHD_create_response_from_buffer(strlen(page), page, MHD_RESPMEM_MUST_FREE);
        if (!response) {
                free(page);
                return respond_oom(connection);
        }

        MHD_add_response_header(response, "Content-Type", "text/html");
        MHD_add_response_header(response, "Location", target);

        ret = MHD_queue_response(connection, MHD_HTTP_MOVED_PERMANENTLY, response);
        MHD_destroy_response(response);

        return ret;
}

static int request_handler_file(
                struct MHD_Connection *connection,
                const char *path,
                const char *mime_type) {

        struct MHD_Response *response;
        int ret;
        _cleanup_close_ int fd = -1;
        struct stat st;

        assert(connection);
        assert(path);
        assert(mime_type);

        fd = open(path, O_RDONLY|O_CLOEXEC);
        if (fd < 0)
                return respond_error(connection, MHD_HTTP_NOT_FOUND, "Failed to open file %s: %m\n", path);

        if (fstat(fd, &st) < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to stat file: %m\n");

        response = MHD_create_response_from_fd_at_offset(st.st_size, fd, 0);
        if (!response)
                return respond_oom(connection);

        fd = -1;

        MHD_add_response_header(response, "Content-Type", mime_type);

        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);

        return ret;
}

static int request_handler_machine(
                struct MHD_Connection *connection,
                void *connection_cls) {

        struct MHD_Response *response;
        RequestMeta *m = connection_cls;
        int r;
        _cleanup_free_ char* hostname = NULL, *os_name = NULL;
        uint64_t cutoff_from, cutoff_to, usage;
        char *json;
        sd_id128_t mid, bid;
        const char *v = "bare";

        assert(connection);
        assert(m);

        r = open_journal(m);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to open journal: %s\n", strerror(-r));

        r = sd_id128_get_machine(&mid);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to determine machine ID: %s\n", strerror(-r));

        r = sd_id128_get_boot(&bid);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to determine boot ID: %s\n", strerror(-r));

        hostname = gethostname_malloc();
        if (!hostname)
                return respond_oom(connection);

        r = sd_journal_get_usage(m->journal, &usage);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to determine disk usage: %s\n", strerror(-r));

        r = sd_journal_get_cutoff_realtime_usec(m->journal, &cutoff_from, &cutoff_to);
        if (r < 0)
                return respond_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "Failed to determine disk usage: %s\n", strerror(-r));

        parse_env_file("/etc/os-release", NEWLINE, "PRETTY_NAME", &os_name, NULL);

        detect_virtualization(&v);

        r = asprintf(&json,
                     "{ \"machine_id\" : \"" SD_ID128_FORMAT_STR "\","
                     "\"boot_id\" : \"" SD_ID128_FORMAT_STR "\","
                     "\"hostname\" : \"%s\","
                     "\"os_pretty_name\" : \"%s\","
                     "\"virtualization\" : \"%s\","
                     "\"usage\" : \"%llu\","
                     "\"cutoff_from_realtime\" : \"%llu\","
                     "\"cutoff_to_realtime\" : \"%llu\" }\n",
                     SD_ID128_FORMAT_VAL(mid),
                     SD_ID128_FORMAT_VAL(bid),
                     hostname_cleanup(hostname),
                     os_name ? os_name : "Linux",
                     v,
                     (unsigned long long) usage,
                     (unsigned long long) cutoff_from,
                     (unsigned long long) cutoff_to);

        if (r < 0)
                return respond_oom(connection);

        response = MHD_create_response_from_buffer(strlen(json), json, MHD_RESPMEM_MUST_FREE);
        if (!response) {
                free(json);
                return respond_oom(connection);
        }

        MHD_add_response_header(response, "Content-Type", "application/json");
        r = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);

        return r;
}

static int request_handler(
                void *cls,
                struct MHD_Connection *connection,
                const char *url,
                const char *method,
                const char *version,
                const char *upload_data,
                size_t *upload_data_size,
                void **connection_cls) {

        assert(connection);
        assert(connection_cls);
        assert(url);
        assert(method);

        if (!streq(method, "GET"))
                return MHD_NO;

        if (!*connection_cls) {
                if (!request_meta(connection_cls))
                        return respond_oom(connection);
                return MHD_YES;
        }

        if (streq(url, "/"))
                return request_handler_redirect(connection, "/browse");

        if (streq(url, "/entries"))
                return request_handler_entries(connection, *connection_cls);

        if (startswith(url, "/fields/"))
                return request_handler_fields(connection, url + 8, *connection_cls);

        if (streq(url, "/browse"))
                return request_handler_file(connection, DOCUMENT_ROOT "/browse.html", "text/html");

        if (streq(url, "/machine"))
                return request_handler_machine(connection, *connection_cls);

        return respond_error(connection, MHD_HTTP_NOT_FOUND, "Not found.\n");
}

static char *key_pem = NULL;
static char *cert_pem = NULL;

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_KEY,
                ARG_CERT,
        };

        int r, c;

        static const struct option options[] = {
                { "version", no_argument,       NULL, ARG_VERSION },
                { "key",     required_argument, NULL, ARG_KEY     },
                { "cert",    required_argument, NULL, ARG_CERT    },
                { NULL,      0,                 NULL, 0           }
        };

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "", options, NULL)) >= 0)
                switch(c) {
                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case ARG_KEY:
                        if (key_pem) {
                                log_error("Key file specified twice");
                                return -EINVAL;
                        }
                        r = read_full_file(optarg, &key_pem, NULL);
                        if (r < 0) {
                                log_error("Failed to read key file: %s", strerror(-r));
                                return r;
                        }
                        assert(key_pem);
                        break;

                case ARG_CERT:
                        if (cert_pem) {
                                log_error("Certificate file specified twice");
                                return -EINVAL;
                        }
                        r = read_full_file(optarg, &cert_pem, NULL);
                        if (r < 0) {
                                log_error("Failed to read certificate file: %s", strerror(-r));
                                return r;
                        }
                        assert(cert_pem);
                        break;

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }

        if (optind < argc) {
                log_error("This program does not take arguments.");
                return -EINVAL;
        }

        if (!!key_pem != !!cert_pem) {
                log_error("Certificate and key files must be specified together");
                return -EINVAL;
        }

        return 1;
}

int main(int argc, char *argv[]) {
        struct MHD_Daemon *d = NULL;
        int r, n;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r < 0)
                return EXIT_FAILURE;
        if (r == 0)
                return EXIT_SUCCESS;

        n = sd_listen_fds(1);
        if (n < 0) {
                log_error("Failed to determine passed sockets: %s", strerror(-n));
                goto finish;
        } else if (n > 1) {
                log_error("Can't listen on more than one socket.");
                goto finish;
        } else {
                struct MHD_OptionItem opts[] = {
                        { MHD_OPTION_NOTIFY_COMPLETED,
                          (intptr_t) request_meta_free, NULL },
                        { MHD_OPTION_EXTERNAL_LOGGER,
                          (intptr_t) microhttpd_logger, NULL },
                        { MHD_OPTION_END, 0, NULL },
                        { MHD_OPTION_END, 0, NULL },
                        { MHD_OPTION_END, 0, NULL },
                        { MHD_OPTION_END, 0, NULL }};
                int opts_pos = 2;
                int flags = MHD_USE_THREAD_PER_CONNECTION|MHD_USE_POLL|MHD_USE_DEBUG;

                if (n > 0)
                        opts[opts_pos++] = (struct MHD_OptionItem)
                                {MHD_OPTION_LISTEN_SOCKET, SD_LISTEN_FDS_START};
                if (key_pem) {
                        assert(cert_pem);
                        opts[opts_pos++] = (struct MHD_OptionItem)
                                {MHD_OPTION_HTTPS_MEM_KEY, 0, key_pem};
                        opts[opts_pos++] = (struct MHD_OptionItem)
                                {MHD_OPTION_HTTPS_MEM_CERT, 0, cert_pem};
                        flags |= MHD_USE_SSL;
                }

                d = MHD_start_daemon(flags, 19531,
                                     NULL, NULL,
                                     request_handler, NULL,
                                     MHD_OPTION_ARRAY, opts,
                                     MHD_OPTION_END);
        }

        if (!d) {
                log_error("Failed to start daemon!");
                goto finish;
        }

        pause();

        r = EXIT_SUCCESS;

finish:
        if (d)
                MHD_stop_daemon(d);

        return r;
}
