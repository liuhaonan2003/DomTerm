#include "server.h"
#include <limits.h>
#include <sys/stat.h>
#include <termios.h>
#include <utmp.h>

#define BUF_SIZE 1024

#define USE_RXFLOW (LWS_LIBRARY_VERSION_NUMBER >= (2*1000000+4*1000))
#define UNCONFIRMED_LIMIT 8000

#if defined(TIOCPKT)
// See https://stackoverflow.com/questions/21641754/when-pty-pseudo-terminal-slave-fd-settings-are-changed-by-tcsetattr-how-ca
#define USE_PTY_PACKET_MODE 1
#endif

extern char **environ;

static char eof_message[] = URGENT_START_STRING "\033[99;99u" URGENT_END_STRING;
#define eof_len (sizeof(eof_message)-1)
static char request_contents_message[] =
    OUT_OF_BAND_START_STRING "\033[81u" URGENT_END_STRING;
#define URGENT_WRAP(STR)  URGENT_START_STRING STR URGENT_END_STRING

static char start_replay_mode[] = "\033[97u";
static char end_replay_mode[] = "\033[98u";

struct pty_client *pty_client_list;
struct pty_client *pty_client_last;

int
send_initial_message(struct lws *wsi) {
#if 0
    unsigned char message[LWS_PRE + 256];
    unsigned char *p = &message[LWS_PRE];
    int n;

    char hostname[128];
    gethostname(hostname, sizeof(hostname) - 1);

    // window title
    n = sprintf((char *) p, "%c%s (%s)", SET_WINDOW_TITLE, server->command, hostname);
    if (lws_write(wsi, p, (size_t) n, LWS_WRITE_TEXT) < n) {
        return -1;
    }
    // reconnect time
    n = sprintf((char *) p, "%c%d", SET_RECONNECT, server->reconnect);
    if (lws_write(wsi, p, (size_t) n, LWS_WRITE_TEXT) < n) {
        return -1;
    }
#endif
    return 0;
}

#if 0
bool
check_host_origin(struct lws *wsi) {
    int origin_length = lws_hdr_total_length(wsi, WSI_TOKEN_ORIGIN);
    char buf[origin_length + 1];
    memset(buf, 0, sizeof(buf));
    int len = lws_hdr_copy(wsi, buf, sizeof(buf), WSI_TOKEN_ORIGIN);
    if (len > 0) {
        const char *prot, *address, *path;
        int port;
        if (lws_parse_uri(buf, &prot, &address, &port, &path))
            return false;
        sprintf(buf, "%s:%d", address, port);
        int host_length = lws_hdr_total_length(wsi, WSI_TOKEN_HOST);
        if (host_length != strlen(buf))
            return false;
        char host_buf[host_length + 1];
        memset(host_buf, 0, sizeof(host_buf));
        len = lws_hdr_copy(wsi, host_buf, sizeof(host_buf), WSI_TOKEN_HOST);
        return len > 0 && strcasecmp(buf, host_buf) == 0;
    }
    return false;
}
#endif

bool
should_backup_output(struct pty_client *pclient)
{
    struct lws *twsi;
    FOREACH_WSCLIENT(twsi, pclient) {
      struct tty_client *tclient = (struct tty_client *) lws_wsi_user(twsi);
      if (tclient->requesting_contents == 2)
          return true;
      if (! tclient->detach_on_close)
          return false;
    }
    return true;
}

void
maybe_exit()
{
    if (server->session_count + server->client_count == 0) {
        force_exit = true;
        lws_cancel_service(context);
        exit(0);
    }
}

void
pty_destroy(struct pty_client *pclient)
{
    struct pty_client **p = &pty_client_list;
    struct pty_client *prev = NULL;
    for (;*p != NULL; p = &(*p)->next_pty_client) {
      if (*p == pclient) {
        *p = pclient->next_pty_client;
        if (pty_client_last == pclient)
          pty_client_last = prev;
        break;
      }
      prev = *p;
    }

    // stop event loop
    pclient->exit = true;
    if (pclient->ttyname != NULL) {
        free(pclient->ttyname);
        pclient->ttyname = NULL;
    }
    if (pclient->saved_window_contents != NULL) {
        free(pclient->saved_window_contents);
        pclient->saved_window_contents = NULL;
    }
    if (pclient->preserved_output != NULL) {
        free(pclient->preserved_output);
        pclient->preserved_output = NULL;
    }

    // kill process and free resource
    lwsl_notice("sending %d to process %d\n",
                server->options.sig_code, pclient->pid);
    if (kill(pclient->pid, server->options.sig_code) != 0) {
        lwsl_err("kill: pid: %d, errno: %d (%s)\n", pclient->pid, errno, strerror(errno));
    }
    int status;
    while (waitpid(pclient->pid, &status, 0) == -1 && errno == EINTR)
        ;
    lwsl_notice("process exited with code %d, pid: %d\n", status, pclient->pid);
    close(pclient->pty);
#ifndef LWS_TO_KILL_SYNC
#define LWS_TO_KILL_SYNC (-1)
#endif
    // FIXME free client; set pclient to NULL in all matching tty_clients.

    // remove from sessions list
    server->session_count--;
    maybe_exit();
}

void
printf_to_browser(struct tty_client *tclient, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    sbuf_vprintf(&tclient->ob, format, ap);
    va_end(ap);
}

void
tty_client_destroy(struct lws *wsi, struct tty_client *tclient) {
    sbuf_free(&tclient->ob);

    // remove from clients list
    server->client_count--;
    maybe_exit();

    struct pty_client *pclient = tclient->pclient;
    if (pclient == NULL)
        return;

    //if (pclient->exit || pclient->pid <= 0)
    //    return;
    // Unlink wsi from pclient's list of client_wsi-s.
    for (struct lws **pwsi = &pclient->first_client_wsi; *pwsi != NULL; ) {
      struct lws **nwsi = &((struct tty_client *) lws_wsi_user(*pwsi))->next_client_wsi;
      if (wsi == *pwsi) {
        if (*nwsi == NULL)
          pclient->last_client_wsi_ptr = pwsi;
        *pwsi = *nwsi;
        break;
      }
      pwsi = nwsi;
    }
    // FIXME reclaim memory cleanup for tclient
    struct lws *first_twsi = pclient->first_client_wsi;
    if (tclient->detach_on_close) {
        pclient->detach_count++;
    } else if (first_twsi == NULL && pclient->detach_count == 0) {
        lws_set_timeout(pclient->pty_wsi, PENDING_TIMEOUT_SHUTDOWN_FLUSH, LWS_TO_KILL_SYNC);
    }
    // If only one client left, do detachSaveSend
    if (first_twsi != NULL) {
        struct tty_client *first_tclient = lws_wsi_user(first_twsi);
        if (first_tclient->next_client_wsi == NULL) {
            first_tclient->pty_window_number = -1;
            first_tclient->pty_window_update_needed = true;
            first_tclient->detachSaveSend = true;
        }
    }
}

static void
setWindowSize(struct pty_client *client)
{
    struct winsize ws;
    ws.ws_row = client->nrows;
    ws.ws_col = client->ncols;
    ws.ws_xpixel = (int) client->pixw;
    ws.ws_ypixel = (int) client->pixh;
    if (ioctl(client->pty, TIOCSWINSZ, &ws) < 0)
        lwsl_err("ioctl TIOCSWINSZ: %d (%s)\n", errno, strerror(errno));
}

void link_command(struct lws *wsi, struct tty_client *tclient,
                  struct pty_client *pclient)
{
    tclient->pclient = pclient;
    struct lws *first_twsi = pclient->first_client_wsi;
    tclient->next_client_wsi = NULL;

    if (first_twsi != NULL) {
        struct tty_client *first_tclient = lws_wsi_user(first_twsi);
        if (first_tclient->next_client_wsi == NULL)
            first_tclient->pty_window_number = 0;

        // Find the lowest unused pty_window_number.
        // This is O(n^2), but typically n==0.
        struct lws *xwsi;
        int n = -1;
    next_pty_window_number:
        n++;
        FOREACH_WSCLIENT(xwsi, pclient) {
          struct tty_client *xclient = (struct tty_client *) lws_wsi_user(xwsi);
          if (xclient->pty_window_number == n)
              goto next_pty_window_number;
        }
        tclient->pty_window_number = n;

        // If following this link_command there are now two clients,
        // notify both clients they don't have to save on detatch
        if (first_tclient->next_client_wsi == NULL) {
            first_tclient->pty_window_update_needed = true;
            // these was exctly one other tclient
            tclient->detachSaveSend = true;
            lws_callback_on_writable(wsi);
            first_tclient->detachSaveSend = true;
            lws_callback_on_writable(first_twsi);
        }
    } else if (pclient->detachOnClose) // FIXME
        tclient->detachSaveSend = true;
    tclient->pty_window_update_needed = true;
    *pclient->last_client_wsi_ptr = wsi;
    pclient->last_client_wsi_ptr = &tclient->next_client_wsi;
    focused_wsi = wsi;
    if (pclient->detached)
        pclient->detachOnClose = 0; // OLD
    pclient->detached = 0;
    if (pclient->detach_count > 0)
        pclient->detach_count--;
    if (pclient->paused) {
#if USE_RXFLOW
        lws_rx_flow_control(pclient->pty_wsi,
                            1|LWS_RXFLOW_REASON_FLAG_PROCESS_NOW);
#endif
        pclient->paused = 0;
    }
}

void put_to_env_array(char **arr, int max, char* eval)
{
    char *eq = index(eval, '=');
    int name_len = eq - eval;
    for (int i = 0; ; i++) {
        if (arr[i] == NULL) {
            if (i == max)
                abort();
            arr[i] = eval;
            arr[i+1] = NULL;
        }
        if (strncmp(arr[i], eval, name_len+1) == 0) {
            arr[i] = eval;
            break;
        }
    }
}

static struct pty_client *
run_command(const char *cmd, char*const*argv, const char*cwd,
            char **env, struct options *opts)
{
    struct lws *outwsi;
    int session_number = ++last_session_number;

    int master;
    int slave;
    bool packet_mode = false;

    if (openpty(&master, &slave,NULL, NULL, NULL)) {
        lwsl_err("openpty\n");
        return NULL;
    }
#if USE_PTY_PACKET_MODE
    if (! (opts->tty_packet_mode
           && strcmp(opts->tty_packet_mode, "no") == 0)) {
        int nonzero = 1;
        packet_mode = ioctl(master, TIOCPKT, &nonzero) == 0;
    }
#if EXTPROC
    if (packet_mode
        && (opts->tty_packet_mode == NULL
            || strcmp(opts->tty_packet_mode, "extproc") == 0)) {
        struct termios tio;
        tcgetattr(slave, &tio);
        tio.c_lflag |= EXTPROC;
        tcsetattr(slave, TCSANOW, &tio);
    }
#endif
#endif

            pid_t pid = fork();
    switch (pid) {
    case -1: /* error */
            lwsl_err("forkpty\n");
            close(master);
            close(slave);
            break;
    case 0: /* child */
            close(master);
            if (login_tty(slave))
                _exit(1);
            if (cwd == NULL || chdir(cwd) != 0) {
                const char *home = find_home();
                if (home == NULL || chdir(home) != 0)
                    if (chdir("/") != 0)
                        lwsl_err("chdir failed\n");
            }
            int env_size = 0;
            while (env[env_size] != NULL) env_size++;
            int env_max = env_size + 10;
            char **nenv = xmalloc((env_max + 1)*sizeof(const char*));
            memcpy(nenv, env, (env_size + 1)*sizeof(const char*));

            put_to_env_array(nenv, env_max, "TERM=xterm-256color");
#if !  WITH_XTERMJS
            put_to_env_array(nenv, env_max, "COLORTERM=truecolor");
            char* dinit = "DOMTERM=";
#ifdef LWS_LIBRARY_VERSION
#define SHOW_LWS_LIBRARY_VERSION "=" LWS_LIBRARY_VERSION
#else
#define SHOW_LWS_LIBRARY_VERSION ""
#endif
            const char *lstr = ";libwebsockets" SHOW_LWS_LIBRARY_VERSION;
            char* pinit = ";tty=";
            char* ttyName = ttyname(0);
            char pidbuf[40];
            pid = getpid();
            size_t dlen = strlen(dinit);
            size_t llen = strlen(lstr);
            size_t plen = strlen(pinit);
            int tlen = ttyName == NULL ? 0 : strlen(ttyName);
            char *version_info =
              /* FIXME   tclient != NULL ? tclient->version_info
                    :*/ "version=" LDOMTERM_VERSION;
            int vlen = version_info == NULL ? 0 : strlen(version_info);
            int mlen = dlen + vlen + llen + (tlen > 0 ? plen + tlen : 0);
            if (pid > 0) {
                sprintf(pidbuf, ";session#=%d;pid=%d", session_number, pid);
                mlen += strlen(pidbuf);
            }
            char* ebuf = malloc(mlen+1);
            strcpy(ebuf, dinit);
            int offset = dlen;
            if (version_info)
                strcpy(ebuf+offset, version_info);
            offset += vlen;
            strcpy(ebuf+offset, lstr);
            offset += llen;
            if (tlen > 0) {
                strcpy(ebuf+offset, pinit);
                offset += plen;
                strcpy(ebuf+offset, ttyName);
                offset += tlen;
            }
            if (pid > 0) {
                strcpy(ebuf+offset, pidbuf);
                offset += strlen(pidbuf);
            }
            ebuf[mlen] = '\0';
            put_to_env_array(nenv, env_max, ebuf);
#endif
#if ENABLE_LD_PRELOAD
            int normal_user = getuid() == geteuid();
            char* domterm_home = get_bin_relative_path("");
            if (normal_user && domterm_home != NULL) {
#if __APPLE__
                char *fmt =  "DYLD_INSERT_LIBRARIES=%s/lib/domterm-preloads.dylib";
#else
                char *fmt =  "LD_PRELOAD=%s/lib/domterm-preloads.so libdl.so.2";
#endif
                char *buf = malloc(strlen(domterm_home)+strlen(fmt)-1);
                sprintf(buf, fmt, domterm_home);
                put_to_env_array(nenv, env_max, buf);
            }
#endif
            if (execve(cmd, argv, nenv) < 0) {
                perror("execvp");
                exit(1);
            }
            break;
        default: /* parent */
            lwsl_notice("started process, pid: %d\n", pid);
            char *tname = strdup(ttyname(slave));
            close(slave);
            lws_sock_file_fd_type fd;
            fd.filefd = master;
            //if (tclient == NULL)              return NULL;
            outwsi = lws_adopt_descriptor_vhost(vhost, 0, fd, "pty", NULL);
            struct pty_client *pclient = (struct pty_client *) lws_wsi_user(outwsi);
            pclient->ttyname = tname;
            pclient->packet_mode = packet_mode;
            pclient->next_pty_client = NULL;
            server->session_count++;
            if (pty_client_last == NULL)
              pty_client_list = pclient;
            else
              pty_client_last->next_pty_client = pclient;
            pty_client_last = pclient;

            pclient->pid = pid;
            pclient->pty = master;
            pclient->nrows = -1;
            pclient->ncols = -1;
            pclient->pixh = -1;
            pclient->pixw = -1;
            pclient->eof_seen = 0;
            pclient->detachOnClose = 0;
            pclient->detach_count = 0;
            pclient->detached = 0;
            pclient->paused = 0;
            pclient->saved_window_contents = NULL;
            pclient->preserved_output = NULL;
            pclient->first_client_wsi = NULL;
            pclient->last_client_wsi_ptr = &pclient->first_client_wsi;
            pclient->recent_tclient = NULL;
            if (pclient->nrows >= 0)
               setWindowSize(pclient);
            pclient->session_number = session_number;
            pclient->session_name_unique = false;
            pclient->pty_wsi = outwsi;
            // lws_change_pollfd ??
            // FIXME do on end: tty_client_destroy(client);
            return pclient;
    }

    return NULL;
}

struct pty_client *
find_session(const char *specifier)
{
    struct pty_client *session = NULL;
    struct pty_client *pclient = pty_client_list;
    char *pend;
    pid_t pid = strtol(specifier, &pend, 10);
    if (*pend != '\0' || *specifier == '\0')
        pid = -1;

    for (; pclient != NULL; pclient = pclient->next_pty_client) {
        int match = 0;
        if (pclient->pid == pid && pid != -1)
            return pclient;
        if (pclient->session_name != NULL
            && strcmp(specifier, pclient->session_name) == 0)
            match = 1;
        else if (specifier[0] == ':'
                 && strtol(specifier+1, NULL, 10) == pclient->session_number)
          match = 1;
        if (match) {
          if (session != NULL)
            return NULL; // ambiguous
          else
            session = pclient;
        }
    }
    return session;
}

static char localhost_localdomain[] = "localhost.localdomain";

char *
check_template(const char *template, json_object *obj)
{
    struct json_object *jfilename = NULL, *jposition = NULL, *jhref = NULL;
    const char *filename =
        json_object_object_get_ex(obj, "filename", &jfilename)
        ? json_object_get_string(jfilename) : NULL;
    const char *position =
        json_object_object_get_ex(obj, "position", &jposition)
        ? json_object_get_string(jposition) : NULL;
    const char *href =
        json_object_object_get_ex(obj, "href", &jhref)
        ? json_object_get_string(jhref) : NULL;
    if (filename != NULL && filename[0] == '/' && filename[1] == '/') {
        if (filename[2] == '/')
            filename = filename + 2;
        else {
            char *colon = strchr(filename+2, '/');
            if (colon == NULL)
                return NULL;
            int fhlen = colon - (filename + 2);
            if (fhlen == sizeof(localhost_localdomain)-1
                && strncmp(filename+2, localhost_localdomain, fhlen) == 0)
                filename = filename + 2 + fhlen;
            else {
#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 128
#endif
                char hbuf[HOST_NAME_MAX+1];
                int r = gethostname(hbuf, sizeof(hbuf));
                if (r != 0)
                    return NULL;
                if (fhlen == strlen(hbuf)
                    && strncmp(filename+2, hbuf, fhlen) == 0)
                    filename = filename + 2 + fhlen;
                else
                    return NULL;
            }
        }
    }

    int size = 0;
    while (template[0] == '{') {
        // a disjunction of clauses, separated by '|'
        bool ok = false; // true when a previous clause was true
        const char *clause = &template[1];
        for (const char *p = clause; ; p++) {
            char ch = *p;
            if (ch == '\0')
              return NULL;
            if (ch == '|' || ch == '}') {
                if (! ok) {
                    bool negate = *clause=='!';
                    if (negate)
                      clause++;
                    int clen = p - clause;
                    if (clen > 1 && p[-1] == ':') {
                        ok = strncmp(clause, href, clen) == 0;
                    } else if (clause[0] == '.') {
                        const char *h = href;
                        const char *dot = NULL;
                        for (;*h && *h != '#' && *h != '?'; h++)
                          if (*h == '.')
                            dot = h;
                        ok = dot && clen == h - dot
                          && memcmp(dot, clause, clen) == 0;
                    } else if (clen == 7
                               && memcmp(clause, "in-atom", clen) == 0) {
                        struct json_object *jatom = NULL;
                        ok = json_object_object_get_ex(obj, "isAtom", &jatom)
                          && json_object_get_boolean(jatom);
                    } else if (clen == 13
                               && memcmp(clause, "with-position", clen) == 0) {
                        ok = position != NULL;
                    }
                    if (negate)
                        ok = ! ok;
                }
                clause = p + 1;
            }
            if (ch == '}') {
              if (! ok)
                return NULL;
              template = clause;
              break;
            }
        }
    }
    if (strcmp(template, "atom") == 0)
        template = "atom '%F'%:P";
    else if (strcmp(template, "emacs") == 0)
        template = "emacs %+P '%F' > /dev/null 2>&1 ";
    else if (strcmp(template, "emacsclient") == 0)
        template = "emacsclient -n %+P '%F'";
    else if (strcmp(template, "firefox") == 0
             || strcmp(template, "chrome") == 0
             || strcmp(template, "google-chrome") == 0) {
        char *chr = strcmp(template, "firefox") == 0
          ? firefox_browser_command()
          : chrome_command(false);
        if (chr == NULL)
            return NULL;
        char *buf = xmalloc(strlen(chr) + strlen(href)+4);
        sprintf(buf, "%s '%s'", chr, href);
        return buf;
    }
    int i;
    for (i = 0; template[i]; i++) {
        char ch = template[i];
        if (ch == '%' && template[i+1]) {
            char next = template[++i];
            char prefix = 0;
            if ((next == ':' || next == '+') && template[i+1]) {
                prefix = next;
                next = template[++i];
            }
            const char *field;
            if (next == 'F') field = filename;
            else if (next == 'U') field = href;
            else if (next == 'P') field = position;
            else field = "%";
            if (field != NULL)
              size += strlen(field) + (prefix ? 1 : 0);
            else if (! prefix)
                return NULL;
        } else
          size++;
    }
    char *buffer = xmalloc(size+1);
    i = 0;
    char *p = buffer;
    for (i = 0; template[i]; i++) {
        char ch = template[i];
        if (ch == '%' && template[i+1]) {
            char next = template[++i];
            char prefix = 0;
            if ((next == ':' || next == '+') && template[i+1]) {
                prefix = next;
                next = template[++i];
            }
            char t2[2];
            const char *field;
            if (next == 'F') field = filename;
            else if (next == 'U') field = href;
            else if (next == 'P') field = position;
            else {
                t2[0] = ch;
                t2[1] = 0;
                field = t2;
            }
            if (field != NULL) {
                if (prefix)
                    *p++ = prefix;
                strcpy(p, field);
                p += strlen(field);
            }
        } else {
            if (ch == ' ') {
                *p = 0;
                if (strchr(buffer, ' ') == NULL) {
                    char* ex = find_in_path(buffer);
                    free(ex == NULL ? buffer : ex);
                    if (ex == NULL)
                        return NULL;
                }
            }
            *p++ = ch;
        }
    }
    *p = 0;
    return buffer;
}

bool
handle_tlink(char *template, json_object *obj)
{
    char *t = strdup(template);
    char *p = t;
    char *command = NULL;
    for (;;) {
        const char *start = NULL;
        char *semi = (char*)
            extract_command_from_list(p, &start, NULL, NULL);
        if (*semi)
            *semi = 0;
        else
            semi = NULL;
        command = check_template(start, obj);
        if (command != NULL)
           break;
        if (semi)
            p = semi+1;
        else
            break;
    }
    free(t);
    if (command == NULL)
        return false;
    if (strcmp(command, "browser")==0||strcmp(command, "default")==0) {
        struct json_object *jhref;
        free(command);
        if (json_object_object_get_ex(obj, "href", &jhref)) {
            default_link_command(json_object_get_string(jhref));
            return true;
        }
    }
    bool r = start_command(main_options, command) == EXIT_SUCCESS;
    free(command);
    return r;
}

void
handle_link(json_object *obj)
{
    if (json_object_object_get_ex(obj, "filename", NULL)) {
        char *template = main_options->openfile_application;
        if (template == NULL)
            template =
              "{in-atom}{with-position|!.html}atom;"
              "{with-position|!.html}emacsclient;"
              "{with-position|!.html}emacs;"
              "{with-position|!.html}atom";
        if (handle_tlink(template, obj))
            return;
    }
    char *template = main_options->openlink_application;
    if (template == NULL)
        template = "{!mailto:}browser;{!mailto:}chrome;{!mailto:}firefox";
    handle_tlink(template, obj);
}

void
reportEvent(const char *name, char *data, size_t dlen,
            struct lws *wsi, struct tty_client *client)
{
    struct pty_client *pclient = client->pclient;
    // FIXME call reportEvent(cname, data)
    if (strcmp(name, "WS") == 0) {
        if (pclient != NULL
            && sscanf(data, "%d %d %g %g", &pclient->nrows, &pclient->ncols,
                      &pclient->pixh, &pclient->pixw) == 4) {
          if (pclient->pty >= 0)
            setWindowSize(pclient);
        }
    } else if (strcmp(name, "VERSION") == 0) {
        char *version_info = xmalloc(dlen+1);
        strcpy(version_info, data);
        client->initialized = false;
        client->version_info = version_info;
        if (pclient == NULL) {
            char** argv = default_command(main_options);
            char *cmd = find_in_path(argv[0]);
            if (cmd != NULL) {
                pclient = run_command(cmd, argv, ".", environ, main_options);
                free(cmd);
            }
        }
        if (client->pclient == NULL) // FIXME merge with previous?
            link_command(wsi, client, pclient);
        if (pclient->saved_window_contents != NULL)
            lws_callback_on_writable(wsi);
    } else if (strcmp(name, "RECEIVED") == 0) {
        long count;
        sscanf(data, "%ld", &count);
        client->confirmed_count = count;
        if (((client->sent_count - client->confirmed_count) & MASK28) < 1000
            && pclient->paused) {
#if USE_RXFLOW
            lws_rx_flow_control(pclient->pty_wsi,
                                1|LWS_RXFLOW_REASON_FLAG_PROCESS_NOW);
#endif
            pclient->paused = 0;
        }
    } else if (strcmp(name, "KEY") == 0) {
        char *q1 = strchr(data, '\t');
        char *q2;
        if (q1 == NULL || (q2 = strchr(q1+1, '\t')) == NULL)
            return; // ERROR
        struct termios termios;
        if (tcgetattr(pclient->pty, &termios) < 0)
          ; //return -1;
        bool isCanon = (termios.c_lflag & ICANON) != 0;
        bool isEchoing = (termios.c_lflag & ECHO) != 0;
        json_object *obj = json_tokener_parse(q2+1);
        const char *kstr = json_object_get_string(obj);
        int klen = json_object_get_string_len(obj);
        int kstr0 = klen != 1 ? -1 : kstr[0];
        if (isCanon && kstr0 != 3 && kstr0 != 4 && kstr0 != 26) {
            printf_to_browser(client, URGENT_WRAP("\033]%d;%.*s\007"),
                              isEchoing ? 74 : 73, (int) dlen, data);
            lws_callback_on_writable(wsi);
        } else {
          int to_drain = 0;
          if (pclient->paused) {
            struct termios term;
            // If we see INTR, we want to drain already-buffered data.
            // But we don't want to drain data that written after the INTR.
            if (tcgetattr(pclient->pty, &term) == 0
                && term.c_cc[VINTR] == kstr0
                && ioctl (pclient->pty, FIONREAD, &to_drain) != 0)
              to_drain = 0;
          }
          if (write(pclient->pty, kstr, klen) < klen)
             lwsl_err("write INPUT to pty\n");
          while (to_drain > 0) {
            char buf[500];
            ssize_t r = read(pclient->pty, buf,
                             to_drain <= sizeof(buf) ? to_drain : sizeof(buf));
            if (r <= 0)
              break;
            to_drain -= r;
          }
        }
        json_object_put(obj);
    } else if (strcmp(name, "SESSION-NAME") == 0) {
        char *q = strchr(data, '"');
        json_object *obj = json_tokener_parse(q);
        const char *kstr = json_object_get_string(obj);
        int klen = json_object_get_string_len(obj);
        char *session_name = xmalloc(klen+1);
        strcpy(session_name, kstr);
        if (pclient->session_name)
            free(pclient->session_name);
        pclient->session_name = session_name;
        pclient->session_name_unique = true;
        json_object_put(obj);
        for (struct pty_client *p = pty_client_list;
             p != NULL; p = p->next_pty_client) {
            if (p != pclient && p->session_name != NULL
                && strcmp(session_name, p->session_name) == 0) {
                struct lws *twsi;
                struct pty_client *pp = p;
                p->session_name_unique = false;
                for (;;) {
                    FOREACH_WSCLIENT(twsi, pp) {
                        struct tty_client *t =
                            (struct tty_client *) lws_wsi_user(twsi);
                        t->pty_window_update_needed = true;
                        lws_callback_on_writable(twsi);
                    }
                    if (! pclient->session_name_unique || pp == pclient)
                        break;
                    pp = pclient;
                }
                pclient->session_name_unique = false;
            }
        }
    } else if (strcmp(name, "OPEN-WINDOW") == 0) {
        static char gopt[] =  "geometry=";
        char *g = strstr(data, gopt);
        char *geom = NULL;
        if (g != NULL) {
            g += sizeof(gopt)-1;
            char *gend = strstr(g, "&");
            if (gend == NULL)
                gend = g + strlen(g);
            int glen = gend-g;
            geom = xmalloc(glen+1);
            memcpy(geom, g, glen);
            geom[glen] = 0;
        }
        struct options opts;
        init_options(&opts);
        opts.geometry = geom;
        do_run_browser(&opts, data, -1);
        if (geom != NULL)
            free(geom);
    } else if (strcmp(name, "DETACH") == 0) {
        bool val = strcmp(data,"0")!=0;
        if (pclient != NULL) {
            pclient->detachOnClose = val; // OLD
            client->detach_on_close = (bool)val;
            if (pclient->preserved_output == NULL
                && client->requesting_contents == 0)
                client->requesting_contents = 1;
        }
    } else if (strcmp(name, "FOCUSED") == 0) {
        focused_wsi = wsi;
    } else if (strcmp(name, "LINK") == 0) {
        json_object *obj = json_tokener_parse(data);
        handle_link(obj);
        json_object_put(obj);
    } else if (strcmp(name, "WINDOW-CONTENTS") == 0) {
        char *q = strchr(data, ',');
        long rcount;
        sscanf(data, "%ld", &rcount);
        int updated = (rcount - pclient->preserved_sent_count) & MASK28;
        // Roughly: if (rcount < pclient->preserved_sent_count)
        if ((updated & ((MASK28+1)>>1)) != 0) {
            return;
        }
        if (pclient->saved_window_contents != NULL)
            free(pclient->saved_window_contents);
        pclient->saved_window_contents = strdup(q+1);
        client->requesting_contents = 0;

        int old_length = pclient->preserved_end - pclient->preserved_start; 
        if (pclient->preserved_output == NULL)
          ; // do nothing
        else if (updated >= old_length) {
            pclient->preserved_start = PRESERVE_MIN;
            pclient->preserved_end = pclient->preserved_start;
        }
        else if (pclient->preserved_start + updated < 200)
           pclient->preserved_start += updated;
        else {
            int new_length = old_length - updated;
            memmove(pclient->preserved_output + PRESERVE_MIN,
                    pclient->preserved_output+pclient->preserved_start+updated,
                    new_length);
            pclient->preserved_start = PRESERVE_MIN;
            pclient->preserved_end = PRESERVE_MIN + new_length;
        }
        pclient->preserved_sent_count = rcount;
    } else if (strcmp(name, "ECHO-URGENT") == 0) {
        json_object *obj = json_tokener_parse(data);
        const char *kstr = json_object_get_string(obj);
        struct lws *tty_wsi;
        FOREACH_WSCLIENT(tty_wsi, pclient) {
            struct tty_client *t =
                (struct tty_client *) lws_wsi_user(tty_wsi);
            printf_to_browser(t, URGENT_WRAP("%s"), kstr);
            lws_callback_on_writable(tty_wsi);
        }
        json_object_put(obj);
    } else {
    }
}

int
callback_tty(struct lws *wsi, enum lws_callback_reasons reason,
             void *user, void *in, size_t len)
{
    struct tty_client *client = (struct tty_client *) user;
    struct pty_client *pclient = client == NULL ? NULL : client->pclient;
    //fprintf(stderr, "callback_tty reason:%d\n", (int) reason);

    switch (reason) {
    case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
         //fprintf(stderr, "callback_tty FILTER_PROTOCOL_CONNECTION\n");
         if (server->options.once && server->client_count > 0) {
              lwsl_notice("refuse to serve new client due to the --once option.\n");
              return -1;
         }
         break;

    case LWS_CALLBACK_ESTABLISHED:
        client->initialized = false;
        client->detachSaveSend = false;
        client->uploadSettingsNeeded = true;
        client->authenticated = false;
        client->requesting_contents = 0;
        client->wsi = wsi;
        client->buffer = NULL;
        client->version_info = NULL;
        client->pclient = NULL;
        client->sent_count = 0;
        client->confirmed_count = 0;
        sbuf_init(&client->ob);
        sbuf_extend(&client->ob, 2048);
        sbuf_blank(&client->ob, LWS_PRE);
        client->ocount = 0;
        client->detach_on_close = false;
        client->connection_number = ++server->connection_count;
        client->pty_window_number = -1;
        client->pty_window_update_needed = false;
        {
             char arg[100]; // FIXME
             if (! check_server_key(wsi, arg, sizeof(arg) - 1))
                  return -1;
             const char*connect_pid = lws_get_urlarg_by_name(wsi, "connect-pid=", arg, sizeof(arg) - 1);
             int cpid;
             if (connect_pid != NULL
                 && (cpid = strtol(connect_pid, NULL, 10)) != 0) {
                  struct pty_client *pclient = pty_client_list;
                  for (; pclient != NULL; pclient = pclient->next_pty_client) {
                       if (pclient->pid == cpid) {
                            link_command(wsi, client, pclient);
                            break;
                       }
                  }
             }
        }
        lws_get_peer_addresses(wsi, lws_get_socket_fd(wsi),
                               client->hostname, sizeof(client->hostname),
                               client->address, sizeof(client->address));
        // Defer start_pty so we can set up DOMTERM variable with version_info.
        // start_pty(client);

        server->client_count++;

        lwsl_notice("client connected from %s (%s), total: %d\n", client->hostname, client->address, server->client_count);
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
         //fprintf(stderr, "callback_tty CALLBACK_SERVER_WRITEABLE init:%d connect:%d\n", (int) client->initialized, client->connection_number);
        ;
        struct sbuf buf;
        sbuf_init(&buf);
        sbuf_blank(&buf, LWS_PRE);

        if (! client->initialized
            && (pclient->preserved_output == NULL
                || pclient->saved_window_contents != NULL)) {
#define FORMAT_PID_SNUMBER "\033]31;%d\007\033[91;%d;%d\007"
#define FORMAT_SNAME "\033]30;%s\007"
            sbuf_printf(&buf,
                        pclient->session_name
                        ? URGENT_WRAP(FORMAT_PID_SNUMBER FORMAT_SNAME)
                        : URGENT_WRAP(FORMAT_PID_SNUMBER),
                        pclient->pid,
                        pclient->session_number, pclient->session_name_unique,
                        pclient->session_name);
            if (pclient->saved_window_contents != NULL) {
                int rcount = pclient->preserved_sent_count;
                sbuf_printf(&buf,
                            URGENT_WRAP("\033]103;%ld,%s\007"),
                            (long) pclient->preserved_sent_count,
                            (char *) pclient->saved_window_contents);
                if (! should_backup_output(pclient)) {
                    free(pclient->saved_window_contents);
                    pclient->saved_window_contents = NULL;
                }
                if (pclient->preserved_output != NULL) {
                    size_t start = pclient->preserved_start;
                    size_t end = pclient->preserved_end;
                    sbuf_printf(&buf, "%s%.*s%s",
                                start_replay_mode,
                                (int) (end-start),
                                pclient->preserved_output+start,
                                end_replay_mode);
                    rcount += end - start;
                }
                rcount = rcount & MASK28;
                client->sent_count = rcount;
                client->confirmed_count = rcount;
                sbuf_printf(&buf,
                            OUT_OF_BAND_START_STRING "\033[96;%du"
                            URGENT_END_STRING,
                            rcount);
            }
        }
        if (client->pty_window_update_needed) {
            client->pty_window_update_needed = false;
            sbuf_printf(&buf, URGENT_WRAP("\033[91;%d;%d;%du"),
                        pclient->session_number,
                        pclient->session_name_unique,
                        client->pty_window_number+1);
        }
        if (client->uploadSettingsNeeded) {
            client->uploadSettingsNeeded = false;
            if (settings_as_json != NULL) {
                sbuf_printf(&buf, URGENT_WRAP("\033]89;%s\007"),
                            settings_as_json);
            }
        }
        if (client->detachSaveSend) {
            int tcount = 0;
            struct lws *tty_wsi;
            FOREACH_WSCLIENT(tty_wsi, pclient) {
                if (++tcount >= 2) break;
            }
            int code = tcount >= 2 ? 0 : pclient->detachOnClose ? 2 : 1;
            sbuf_printf(&buf, URGENT_WRAP("\033[82;%du"), code);
            client->detachSaveSend = false;
        }
        if (client->ob.len > LWS_PRE) {
            client->sent_count = (client->sent_count + client->ocount) & MASK28;
            sbuf_printf(&buf, "%.*s", (int) client->ob.len-LWS_PRE,
                        client->ob.buffer+LWS_PRE);
            client->ocount = 0;
            if (client->ob.size > 4000) {
                sbuf_free(&client->ob);
                sbuf_extend(&client->ob, 2048);
            }
            client->ob.len = LWS_PRE;
        }
        if (client->requesting_contents == 1) {
            sbuf_printf(&buf, "%s", request_contents_message);
            client->requesting_contents = 2;
            if (pclient->preserved_output == NULL) {
                pclient->preserved_start = PRESERVE_MIN;
                pclient->preserved_end = pclient->preserved_start;
                pclient->preserved_size = 1024;
                pclient->preserved_output =
                    xmalloc(pclient->preserved_size);
            }
            pclient->preserved_sent_count = client->sent_count;
        }
        if (! pclient && client->ob.buffer != NULL) {
            sbuf_printf(&buf, "%s", eof_message);
            sbuf_free(&client->ob);
        }
        int written = buf.len - LWS_PRE;
        if (written > 0
            && lws_write(wsi, buf.buffer+LWS_PRE, written, LWS_WRITE_BINARY) != written)
            lwsl_err("lws_write\n");
        sbuf_free(&buf);
        client->initialized = true;
        break;
    buffer_too_small:
        lwsl_err("bad buffer size\n");
        fprintf(stderr, "bad buffer size\n");
        exit(-1);

    case LWS_CALLBACK_RECEIVE:
         // receive data from websockets client (browser)
         //fprintf(stderr, "callback_tty CALLBACK_RECEIVE len:%d\n", (int) len);
         if (client->buffer == NULL) {
              client->buffer = xmalloc(len + 1);
              client->len = len;
              memcpy(client->buffer, in, len);
         } else {
              client->buffer = xrealloc(client->buffer, client->len + len + 1);
              memcpy(client->buffer + client->len, in, len);
              client->len += len;
         }
         client->buffer[client->len] = '\0';

         // check if there are more fragmented messages
         if (lws_remaining_packet_payload(wsi) > 0 || !lws_is_final_fragment(wsi)) {
              return 0;
         }

         if (server->options.readonly)
              return 0;
         size_t clen = client->len;
         unsigned char *msg = (unsigned char*) client->buffer;
         struct pty_client *pclient = client->pclient;
         if (pclient)
             pclient->recent_tclient = client;
         // FIXME handle PENDING
         int start = 0;
         for (int i = 0; ; i++) {
              if (i+1 == clen && msg[i] >= 128)
                   break;
              // 0x92 (utf-8 0xc2,0x92) "Private Use 2".
              if (i == clen || msg[i] == 0x92
                  || (msg[i] == 0xc2 && msg[i+1] == 0x92)) {
                   int w = i - start;
                   if (w > 0 && write(pclient->pty, msg+start, w) < w) {
                        lwsl_err("write INPUT to pty\n");
                        return -1;
                   }
                   if (i == clen) {
                        start = clen;
                        break;
                   }
                   // look for reported event
                   if (msg[i] == 0xc2)
                        i++;
                   unsigned char* eol = memchr(msg+i, '\n', clen-i);
                   if (eol) {
                        unsigned char *p = msg+i;
                        char* cname = (char*) ++p;
                        while (p < eol && *p != ' ')
                             p++;
                        *p = '\0';
                        if (p < eol)
                             p++;
                        while (p < eol && *p == ' ')
                             p++;
                        // data is from p to eol
                        char *data = (char*) p;
                        *eol = '\0';
                        size_t dlen = eol - p;
                        reportEvent(cname, data, dlen, wsi, client);
                        i = eol - msg;
                   } else {
                        break;
                   }
                   start = i+1;
              }
         }
         if (start < clen) {
              memmove(client->buffer, client->buffer+start, clen-start);
              client->len = clen - start;
         }
         else if (client->buffer != NULL) {
              free(client->buffer);
              client->buffer = NULL;
         }
         break;

    case LWS_CALLBACK_CLOSED:
         if (focused_wsi == wsi)
              focused_wsi = NULL;
         tty_client_destroy(wsi, client);
         lwsl_notice("client disconnected from %s (%s), total: %d\n", client->hostname, client->address, server->client_count);
         if (client->version_info != NULL) {
              free(client->version_info);
              client->version_info = NULL;
         }
         if (client->buffer != NULL) {
              free(client->buffer);
              client->buffer = NULL;
         }
         break;

    case LWS_CALLBACK_PROTOCOL_INIT: /* per vhost */
         break;

    case LWS_CALLBACK_PROTOCOL_DESTROY: /* per vhost */
         break;

    default:
         //fprintf(stderr, "callback_tty default reason:%d\n", (int) reason);
         break;
    }

    return 0;
}

int
display_session(struct options *options, struct pty_client *pclient,
                const char *url, int port)
{
    int session_pid = pclient == NULL ? -1 : pclient->pid;
    const char *browser_specifier = options == NULL ? NULL
      : options->browser_command;
    int paneOp = 0;
    if (browser_specifier != NULL && browser_specifier[0] == '-') {
      if (pclient != NULL && strcmp(browser_specifier, "--detached") == 0) {
          pclient->detached = 1;
          return EXIT_SUCCESS;
      }
       if (strcmp(browser_specifier, "--pane") == 0)
          paneOp = 1;
      else if (strcmp(browser_specifier, "--tab") == 0)
          paneOp = 2;
      else if (strcmp(browser_specifier, "--left") == 0)
          paneOp = 10;
      else if (strcmp(browser_specifier, "--right") == 0)
          paneOp = 11;
      else if (strcmp(browser_specifier, "--above") == 0)
          paneOp = 12;
      else if (strcmp(browser_specifier, "--below") == 0)
          paneOp = 13;
    }
    if (paneOp > 0 && focused_wsi == NULL) {
        browser_specifier = NULL;
        paneOp = 0;
    }
    int r = EXIT_SUCCESS;
    if (paneOp > 0) {
        struct tty_client *tclient = (struct tty_client *) lws_wsi_user(focused_wsi);
        if (pclient != NULL)
             printf_to_browser(tclient, URGENT_WRAP("\033[90;%d;%du"),
                               paneOp, session_pid);
        else
             printf_to_browser(tclient, URGENT_WRAP("\033]%d;%d,%s\007"),
                               -port, paneOp, url);
    } else {
        char *buf = xmalloc(strlen(main_html_url) + (url == NULL ? 60 : strlen(url) + 60));
        if (pclient != NULL)
            sprintf(buf, "%s#connect-pid=%d", main_html_url, pclient->pid);
        else if (port == -105) // view saved file
            sprintf(buf, "%s#view-saved=%s",  main_html_url, url);
        else
            sprintf(buf, "%s", url);
        if (browser_specifier
            && strcmp(browser_specifier, "--print-url") == 0) {
            int ulen = strlen(buf);
            buf[ulen] = '\n';
            if (write(options->fd_out, buf, ulen+1) <= 0)
                lwsl_err("write failed\n");
        } else
            r = do_run_browser(options, buf, port);
        free(buf);
    }
    return r;
}

int new_action(int argc, char** argv, const char*cwd, char **env,
               struct lws *wsi, struct options *opts)
{
    int skip = argc == 0 || index(argv[0], '/') != NULL ? 0 : 1;
    if (skip == 1) {
        optind = 1;
        if (process_options(argc, argv, opts) < 0)
          return EXIT_FAILURE;
        skip = optind;
    }
    char**args = argc == skip ? default_command(opts)
      : (char**)(argv+skip);
    char *argv0 = args[0];
    char *cmd = find_in_path(argv0);
    struct stat sbuf;
    if (cmd == NULL || access(cmd, X_OK) != 0
        || stat(cmd, &sbuf) != 0 || (sbuf.st_mode & S_IFMT) != S_IFREG) {
          FILE *out = fdopen(opts->fd_err, "w");
          fprintf(out, "cannot execute '%s'\n", argv0);
          fclose(out);
          return EXIT_FAILURE;
    }
    struct pty_client *pclient = run_command(cmd, args, cwd, env, opts);
    free(cmd);
    if (opts->session_name) {
        pclient->session_name = strdup(opts->session_name);
        opts->session_name = NULL;
    }
    return display_session(opts, pclient, NULL, http_port);
}

int attach_action(int argc, char** argv, const char*cwd,
                  char **env, struct lws *wsi, struct options *opts)
{
    optind = 1;
    process_options(argc, argv, opts);
    if (optind >= argc) {
        char *msg = "domterm attach: missing session specifier\n";
        if (write(opts->fd_err, msg, strlen(msg)) <= 0)
            lwsl_err("write failed\n");
        return EXIT_FAILURE;
    }
    char *session_specifier = argv[optind];
    struct pty_client *pclient = find_session(session_specifier);
    if (pclient == NULL) {
        FILE *err = fdopen(opts->fd_out, "w");
        fprintf(err, "no session '%s' found \n", session_specifier);
        fclose(err);
        return EXIT_FAILURE;
    }

    // If there is an existing tty_client, request contents from browser,
    // if not already doing do.
    struct lws *tty_wsi;
    FOREACH_WSCLIENT(tty_wsi, pclient) {
        if (((struct tty_client *) lws_wsi_user(tty_wsi))
            ->requesting_contents > 0)
            break;
    }
    if (tty_wsi == NULL && (tty_wsi = pclient->first_client_wsi) != NULL) {
        struct tty_client *tclient =
            (struct tty_client *) lws_wsi_user(tty_wsi);
        tclient->requesting_contents = 1;
        lws_callback_on_writable(tty_wsi);
    }

    display_session(opts, pclient, NULL, http_port);
    return EXIT_SUCCESS;
}

int browse_action(int argc, char** argv, const char*cwd,
                  char **env, struct lws *wsi, struct options *opts)
{
    optind = 1;
    process_options(argc, argv, opts);
    if (optind != argc-1) {
        FILE *err = fdopen(opts->fd_out, "w");
        fprintf(err, optind >= argc ? "domterm browse: missing url\n"
                : "domterm browse: more than one url\n");
        fclose(err);
        return EXIT_FAILURE;
    }
    char *url = argv[optind];
    display_session(opts, NULL, url, -104);
    return EXIT_SUCCESS;
}

void
request_upload_settings()
{
    for (struct pty_client *pclient = pty_client_list;
         pclient != NULL; pclient = pclient->next_pty_client) {
        struct lws *twsi;
        FOREACH_WSCLIENT(twsi, pclient) {
            struct tty_client *tclient = (struct tty_client *) lws_wsi_user(twsi);
            tclient->uploadSettingsNeeded = true;
            lws_callback_on_writable(twsi);
        }
    }
}

int
handle_command(int argc, char** argv, const char*cwd,
               char **env, struct lws *wsi, struct options *opts)
{
    struct command *command = argc == 0 ? NULL : find_command(argv[0]);
    char *domterm_env_value = getenv_from_array("DOMTERM", env);
    if (domterm_env_value) {
        static char pid_key[] = ";pid=";
        int current_session_pid = 0;
        char *p = strstr(domterm_env_value, pid_key);
        if (p)
            sscanf(p+(sizeof(pid_key)-1), "%d", &current_session_pid);
        if (current_session_pid) {
            struct pty_client *pclient = pty_client_list;
            for (; pclient != NULL; pclient = pclient->next_pty_client) {
                if (pclient->pid == current_session_pid) {
                    opts->requesting_session = pclient;
                    break;
                }
            }
        }
    }
    if (command != NULL) {
        return (*command->action)(argc, argv, cwd, env, wsi, opts);
    }
    if (argc == 0 || index(argv[0], '/') != NULL) {
        return new_action(argc, argv, cwd, env, wsi, opts);
    } else {
        // normally caught earlier
        FILE *out = fdopen(opts->fd_err, "w");
        fprintf(out, "domterm: unknown command '%s'\n", argv[0]);
        fclose(out);
        return -1;
    }
    return 0;
}

int
callback_cmd(struct lws *wsi, enum lws_callback_reasons reason,
             void *user, void *in, size_t len) {
    struct cmd_client *cclient = (struct cmd_client *) user;
    int socket;
    switch (reason) {
        case LWS_CALLBACK_RAW_RX_FILE:
            socket = cclient->socket;
            //fprintf(stderr, "callback_cmd RAW_RX reason:%d socket:%d getpid:%d\n", (int) reason, socket, getpid());
            struct sockaddr sa;
            socklen_t slen = sizeof sa;
#ifdef SOCK_CLOEXEC
            int sockfd = accept4(socket, &sa, &slen, SOCK_CLOEXEC);
#else
            int sockfd = accept(socket, &sa, &slen);
#endif
            size_t jblen = 512;
            char *jbuf = xmalloc(jblen);
            int jpos = 0;
            struct options opts;
            init_options(&opts);
            for (;;) {
                if (jblen-jpos < 512) {
                    jblen = (3 * jblen) >> 1;
                    jbuf = xrealloc(jbuf, jblen);
                }
                struct msghdr msg;
                struct iovec iov;
                int myfds[2];
                union u { // for alignment
                    char buf[CMSG_SPACE(sizeof myfds)];
                    struct cmsghdr align;
                } u;
                msg.msg_control = u.buf;
                msg.msg_controllen = sizeof u.buf;
                struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
                cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);
                iov.iov_base = jbuf+jpos;
                iov.iov_len = jblen-jpos;
                msg.msg_name = NULL;
                msg.msg_namelen = 0;
                msg.msg_iov = &iov;
                msg.msg_iovlen = 1;
                msg.msg_flags = 0;
                ssize_t n = recvmsg(sockfd, &msg, 0);
                if (msg.msg_controllen > 0) {
                    memcpy(myfds, CMSG_DATA(cmsg), 2*sizeof(int));
                    opts.fd_out = myfds[0];
                    opts.fd_err = myfds[1];
                }
                if (n <= 0) {
                  break;
                } else if (jbuf[jpos+n-1] == '\f') {
                  jpos += n-1;
                  break;
                }
                jpos += n;
            }
            jbuf[jpos] = 0;
            //fprintf(stderr, "from-client: %d bytes '%.*s'\n", jpos, jpos, jbuf);
            struct json_object *jobj
              = json_tokener_parse(jbuf);
            if (jobj == NULL)
              fatal("json parse fail");
            struct json_object *jcwd = NULL;
            struct json_object *jargv = NULL;
            struct json_object *jenv = NULL;
            const char *cwd = NULL;
            // if (!json_object_object_get_ex(jobj, "cwd", &jcwd))
            //   fatal("jswon no cwd");
            int argc = -1;
            char **argv = NULL;
            char **env = NULL;
            if (json_object_object_get_ex(jobj, "cwd", &jcwd)
                && (cwd = strdup(json_object_get_string(jcwd))) != NULL) {
            }
            if (json_object_object_get_ex(jobj, "argv", &jargv)) {
                argc = json_object_array_length(jargv);
                argv = xmalloc(sizeof(char*) * (argc+1));
                for (int i = 0; i <argc; i++) {
                  argv[i] = strdup(json_object_get_string(json_object_array_get_idx(jargv, i)));
                }
                argv[argc] = NULL;
            }
            if (json_object_object_get_ex(jobj, "env", &jenv)) {
                int nenv = json_object_array_length(jenv);
                env = xmalloc(sizeof(char*) * (nenv+1));
                for (int i = 0; i <nenv; i++) {
                  env[i] = strdup(json_object_get_string(json_object_array_get_idx(jenv, i)));
                }
                env[nenv] = NULL;
            }
            json_object_put(jobj);
            optind = 1;
            process_options(argc, argv, &opts);
            int ret = handle_command(argc-optind, argv+optind,
                                     cwd, env, wsi, &opts);
            close(opts.fd_out);
            close(opts.fd_err);
            char r = (char) ret;
            if (write(sockfd, &r, 1) != 1)
                lwsl_err("write failed\n");
            close(sockfd);
            // FIXME: free argv, cwd, env
            break;
    default:
      //fprintf(stderr, "callback_cmd default reason:%d\n", (int) reason);
            break;
    }

    return 0;
}

int
callback_pty(struct lws *wsi, enum lws_callback_reasons reason,
             void *user, void *in, size_t len) {
    struct pty_client *pclient = (struct pty_client *) user;
    switch (reason) {
        case LWS_CALLBACK_RAW_RX_FILE: {
            //fprintf(stderr, "callback+pty LWS_CALLBACK_RAW_RX_FILE\n");
            struct lws *wsclient_wsi;
            long min_unconfirmed = LONG_MAX;
            int avail = INT_MAX;
            FOREACH_WSCLIENT(wsclient_wsi, pclient) {
                struct tty_client *tclient = (struct tty_client *) lws_wsi_user(wsclient_wsi);
                long unconfirmed =
                  ((tclient->sent_count - tclient->confirmed_count) & MASK28)
                  + tclient->ocount;
                if (unconfirmed < min_unconfirmed)
                  min_unconfirmed = unconfirmed;
                int tavail = tclient->ob.size - tclient->ob.len;
                if (tavail < 1000) {
                    sbuf_extend(&tclient->ob, 1000);
                    tavail = tclient->ob.size - tclient->ob.len;
                }
                if (tavail < avail)
                    avail = tavail;
            }
            if (min_unconfirmed >= UNCONFIRMED_LIMIT || avail == 0 || pclient->paused) {
                if (! pclient->paused) {
#if USE_RXFLOW
                    lws_rx_flow_control(wsi, 0|LWS_RXFLOW_REASON_FLAG_PROCESS_NOW);
#endif
                    pclient->paused = 1;
                }
                break;
            }
            if (avail >= eof_len) {
                char *data_start = NULL;
                int data_length = 0, read_length = 0;
                FOREACH_WSCLIENT(wsclient_wsi, pclient) {
                    struct tty_client *tclient =
                        (struct tty_client *) lws_wsi_user(wsclient_wsi);
                    if (data_start == NULL) {
                        data_start = tclient->ob.buffer+tclient->ob.len;
                        ssize_t n;
                        if (pclient->packet_mode) {
#if USE_PTY_PACKET_MODE
                            // We know data_start > obuffer_raw, so
                            // it's safe to access data_start[-1].
                            char save_byte = data_start[-1];
                            n = read(pclient->pty, data_start-1, avail+1);
                            char pcmd = data_start[-1];
                            data_start[-1] = save_byte;
#if TIOCPKT_IOCTL
                            if (n == 1 && (pcmd & TIOCPKT_IOCTL) != 0) {
                                struct termios tio;
                                tcgetattr(pclient->pty, &tio);
                                const char* icanon_str = (tio.c_lflag & ICANON) != 0 ? "icanon" :  "-icanon";
                                const char* echo_str = (tio.c_lflag & ECHO) != 0 ? "echo" :  "-echo";
                                const char* extproc_str = "";
#if EXTPROC
                                if ((tio.c_lflag & EXTPROC) != 0)
                                    extproc_str = " extproc";
#endif
                                n = sprintf(data_start,
                                            URGENT_WRAP("\033]71; %s %s%s lflag:%x\007"),
                                            icanon_str, echo_str,
                                            extproc_str, tio.c_lflag);
                                data_length = n;
                            }
                            else
#endif
                                read_length = n > 0 ? n - 1 : n;
#endif
                        } else {
                            n = read(pclient->pty, data_start, avail);
                            read_length = n;
                        }
                        data_length += read_length;
                    } else {
                        memcpy(tclient->ob.buffer+tclient->ob.len,
                               data_start, data_length);
                    }
                    tclient->ob.len += data_length;
                    tclient->ocount += read_length;
                    lws_callback_on_writable(wsclient_wsi);
                }
                if (pclient->preserved_output != NULL
                    && should_backup_output(pclient)) {
                     size_t needed = pclient->preserved_end + data_length;
                     if (needed > pclient->preserved_size) {
                          size_t nsize = (3 * pclient->preserved_size) >> 1;
                          if (needed > nsize)
                               nsize = needed;
                          char * nbuffer = xrealloc(pclient->preserved_output, nsize);
                          pclient->preserved_output = nbuffer;
                          pclient->preserved_size = nsize;
                     }
                     memcpy(pclient->preserved_output + pclient->preserved_end,
                            data_start, data_length);
                     pclient->preserved_end += data_length;
                }
            }
        }
        break;
        case LWS_CALLBACK_RAW_CLOSE_FILE: {
            //fprintf(stderr, "callback_pty LWS_CALLBACK_RAW_CLOSE_FILE\n", reason);
            pclient->eof_seen = 1;
            struct lws *wsclient_wsi;
            FOREACH_WSCLIENT(wsclient_wsi, pclient) {
                struct tty_client *tclient =
                    (struct tty_client *) lws_wsi_user(wsclient_wsi);
                lws_callback_on_writable(wsclient_wsi);
                tclient->pclient = NULL;
            }
            pty_destroy(pclient);
        }
        break;
    default:
            //fprintf(stderr, "callback_pty default reason:%d\n", (int) reason);
            break;
    }

    return 0;
}
