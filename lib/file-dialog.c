/* Show the desktop's file chooser for a Windows program running under Wine.
 *
 * Wine's comdlg32 runs this through __wine_unix_spawnvp and waits for OUT.
 * It asks xdg-desktop-portal for a file, so Plasma shows its own file dialog
 * (the one Dolphin's places and settings belong to) and GNOME shows its own.
 *
 * Usage: tuxoffice-file-dialog --out FILE [--save|--open|--folder] [--multiple]
 *            [--title TEXT] [--accept LABEL] [--name NAME] [--current-folder DIR]
 *            [--filter "NAME<TAB>PATTERN;PATTERN"]... [--filter-index N] [--parent PID]
 * The dialog closes by itself when process PID (the Windows program, which
 * is not this helper's parent: Wine detaches spawned programs) exits.
 * OUT is written once, atomically, when the dialog closes:
 *   status ok|cancel|error MESSAGE
 *   filter N          (0-based, the filter selected in the dialog)
 *   path /unix/path   (one line per chosen file)
 *
 * Build: cc -O2 -o tuxoffice-file-dialog lib/file-dialog.c $(pkg-config --cflags --libs libsystemd)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <systemd/sd-bus.h>

#define MAX_FILTERS 64

struct filter { char *name; char *patterns; };

static const char *out_path;
static struct filter filters[MAX_FILTERS];
static int filter_count, filter_index = -1;

struct response
{
    bool done;
    unsigned int code;
    char **paths;
    int path_count;
    int filter;
};

static int write_result(const char *status, const struct response *response)
{
    char *temp;
    FILE *file;
    int i;

    if (asprintf(&temp, "%s.part", out_path) < 0) return 1;
    if (!(file = fopen(temp, "w"))) return 1;
    fprintf(file, "status %s\n", status);
    if (response)
    {
        if (response->filter >= 0) fprintf(file, "filter %d\n", response->filter);
        for (i = 0; i < response->path_count; i++) fprintf(file, "path %s\n", response->paths[i]);
    }
    fclose(file);
    rename(temp, out_path);
    free(temp);
    return 0;
}

static int fail(const char *message)
{
    char *status;

    if (asprintf(&status, "error %s", message) >= 0) write_result(status, NULL);
    fprintf(stderr, "tuxoffice-file-dialog: %s\n", message);
    return 1;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

/* file:///home/user/My%20File.docx -> /home/user/My File.docx */
static char *path_from_uri(const char *uri)
{
    const char *src;
    char *path, *dst;

    if (strncmp(uri, "file://", 7)) return NULL;
    src = uri + 7;
    if (*src != '/')  /* file://localhost/... */
    {
        if (!(src = strchr(src, '/'))) return NULL;
    }
    if (!(path = malloc(strlen(src) + 1))) return NULL;
    for (dst = path; *src; src++)
    {
        if (*src == '%' && hex_value(src[1]) >= 0 && hex_value(src[2]) >= 0)
        {
            *dst++ = hex_value(src[1]) * 16 + hex_value(src[2]);
            src += 2;
        }
        else *dst++ = *src;
    }
    *dst = 0;
    return path;
}

/* $HOME/Documents, from user-dirs.dirs like xdg-user-dir. */
static char *xdg_user_dir(const char *name)
{
    const char *home = getenv("HOME"), *config = getenv("XDG_CONFIG_HOME");
    char *path = NULL, *line = NULL, *key, *ret = NULL;
    size_t size = 0;
    FILE *file;

    if (!home) return NULL;
    if (config && *config) { if (asprintf(&path, "%s/user-dirs.dirs", config) < 0) return NULL; }
    else if (asprintf(&path, "%s/.config/user-dirs.dirs", home) < 0) return NULL;
    if (asprintf(&key, "XDG_%s_DIR=\"", name) < 0) { free(path); return NULL; }
    if ((file = fopen(path, "r")))
    {
        while (!ret && getline(&line, &size, file) > 0)
        {
            char *value, *end;

            if (strncmp(line, key, strlen(key))) continue;
            value = line + strlen(key);
            if (!(end = strchr(value, '"'))) continue;
            *end = 0;
            if (!strncmp(value, "$HOME", 5)) { if (asprintf(&ret, "%s%s", home, value + 5) < 0) ret = NULL; }
            else if (*value == '/') ret = strdup(value);
        }
        fclose(file);
    }
    free(line);
    free(key);
    free(path);
    return ret;
}

/* Office suggests folders of the Windows profile inside the Wine prefix
 * (drive_c/users/NAME/Documents...). Start in the matching Linux folder so
 * that documents are saved where the rest of the desktop finds them. */
static const char *linux_folder(const char *folder)
{
    static const struct { const char *windows, *xdg; } known[] =
    {
        { "Documents", "DOCUMENTS" }, { "Desktop", "DESKTOP" }, { "Downloads", "DOWNLOAD" },
        { "Pictures", "PICTURES" }, { "Music", "MUSIC" }, { "Videos", "VIDEOS" },
    };
    const char *users, *name, *end;
    char *real, *mapped;
    size_t i;

    if (!folder || !(real = realpath(folder, NULL))) return NULL;
    if (!(users = strstr(real, "/drive_c/users/"))) return real;
    name = users + strlen("/drive_c/users/");
    if (!(name = strchr(name, '/'))) return getenv("HOME");
    name++;
    end = strchrnul(name, '/');
    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    {
        if (strlen(known[i].windows) != (size_t)(end - name) || strncmp(name, known[i].windows, end - name))
            continue;
        if ((mapped = xdg_user_dir(known[i].xdg)) && !access(mapped, F_OK)) return mapped;
        free(mapped);
        /* Without user-dirs.dirs, the folders have their English names. */
        if (getenv("HOME") && asprintf(&mapped, "%s/%s", getenv("HOME"), known[i].windows) >= 0)
        {
            if (!access(mapped, F_OK)) return mapped;
            free(mapped);
        }
        break;
    }
    return getenv("HOME");
}

/* Office's patterns are Windows globs: case-insensitive, and "*.*" matches everything. */
static int append_patterns(sd_bus_message *m, const char *patterns)
{
    char *copy = strdup(patterns), *pattern, *save = NULL;
    int r = 0;

    if (!copy) return -1;
    for (pattern = strtok_r(copy, ";", &save); pattern && r >= 0; pattern = strtok_r(NULL, ";", &save))
    {
        char *upper, *p;

        while (*pattern == ' ') pattern++;
        if (!*pattern) continue;
        if (!strcmp(pattern, "*.*") || !strcmp(pattern, "*"))
        {
            r = sd_bus_message_append(m, "(us)", 0, "*");
            continue;
        }
        r = sd_bus_message_append(m, "(us)", 0, pattern);
        if (r < 0 || !(upper = strdup(pattern))) break;
        for (p = upper; *p; p++) *p = toupper((unsigned char)*p);
        if (strcmp(upper, pattern)) r = sd_bus_message_append(m, "(us)", 0, upper);
        free(upper);
    }
    free(copy);
    return r;
}

static int append_filter(sd_bus_message *m, int index)
{
    const struct filter *filter = &filters[index];
    int r, i;

    if ((r = sd_bus_message_open_container(m, 'r', "sa(us)")) < 0) return r;
    if ((r = sd_bus_message_append(m, "s", filter->name)) < 0) return r;
    if ((r = sd_bus_message_open_container(m, 'a', "(us)")) < 0) return r;
    if ((r = append_patterns(m, filter->patterns)) < 0) return r;
    /* Office offers several types for "*.docx". Plasma's dialog identifies the
     * default filter by its patterns, so give each repeat one more that no
     * real file name has. */
    for (i = 0; i < index; i++)
    {
        char *marker;

        if (strcmp(filters[i].patterns, filter->patterns)) continue;
        if (asprintf(&marker, "*.tuxoffice-type-%d", index) < 0) return -1;
        r = sd_bus_message_append(m, "(us)", 0, marker);
        free(marker);
        if (r < 0) return r;
        break;
    }
    if ((r = sd_bus_message_close_container(m)) < 0) return r;
    return sd_bus_message_close_container(m);
}

static int on_response(sd_bus_message *m, void *data, sd_bus_error *error)
{
    struct response *response = data;
    int r;

    response->done = true;
    if ((r = sd_bus_message_read(m, "u", &response->code)) < 0) return r;
    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0)
    {
        const char *key;

        if ((r = sd_bus_message_read(m, "s", &key)) < 0) return r;
        if (!strcmp(key, "uris"))
        {
            const char *uri;

            if ((r = sd_bus_message_enter_container(m, 'v', "as")) < 0) return r;
            if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0) return r;
            while ((r = sd_bus_message_read(m, "s", &uri)) > 0)
            {
                char *path = path_from_uri(uri);
                if (!path) continue;
                response->paths = realloc(response->paths, (response->path_count + 1) * sizeof(char *));
                response->paths[response->path_count++] = path;
            }
            if (r < 0) return r;
            sd_bus_message_exit_container(m);
            sd_bus_message_exit_container(m);
        }
        else if (!strcmp(key, "current_filter"))
        {
            const char *name;
            int i;

            if ((r = sd_bus_message_enter_container(m, 'v', "(sa(us))")) < 0) return r;
            if ((r = sd_bus_message_enter_container(m, 'r', "sa(us)")) < 0) return r;
            if ((r = sd_bus_message_read(m, "s", &name)) < 0) return r;
            for (i = 0; i < filter_count; i++)
                if (!strcmp(filters[i].name, name)) response->filter = i;
            sd_bus_message_skip(m, "a(us)");
            sd_bus_message_exit_container(m);
            sd_bus_message_exit_container(m);
        }
        else sd_bus_message_skip(m, "v");
        sd_bus_message_exit_container(m);
    }
    return r < 0 ? r : 0;
}

static volatile sig_atomic_t stopping;

static void on_signal(int signal)
{
    stopping = 1;
}

/* A pidfd becomes readable when its process exits. */
static bool parent_gone(pid_t parent, int parent_fd)
{
    struct pollfd fd = { .fd = parent_fd, .events = POLLIN };

    if (parent <= 0) return false;
    if (parent_fd >= 0) return poll(&fd, 1, 0) > 0;
    return kill(parent, 0) < 0 && errno == ESRCH;
}

int main(int argc, char **argv)
{
    const char *title = "", *accept = NULL, *name = NULL, *folder = NULL;
    bool save = false, directory = false, multiple = false;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    struct response response = { .filter = -1 };
    sd_bus_message *m = NULL, *reply = NULL;
    char token[64], *request_path, *sender;
    const char *unique, *handle;
    sd_bus *bus = NULL;
    pid_t parent = 0;
    int i, r, parent_fd = -1;

    for (i = 1; i < argc; i++)
    {
        const char *arg = argv[i], *value = i + 1 < argc ? argv[i + 1] : NULL;

        if (!strcmp(arg, "--save")) save = true;
        else if (!strcmp(arg, "--open")) save = false;
        else if (!strcmp(arg, "--folder")) directory = true;
        else if (!strcmp(arg, "--multiple")) multiple = true;
        else if (!value) break;
        else
        {
            i++;
            if (!strcmp(arg, "--out")) out_path = value;
            else if (!strcmp(arg, "--title")) title = value;
            else if (!strcmp(arg, "--accept")) accept = value;
            else if (!strcmp(arg, "--name")) name = value;
            else if (!strcmp(arg, "--current-folder")) folder = value;
            else if (!strcmp(arg, "--filter-index")) filter_index = atoi(value);
            else if (!strcmp(arg, "--parent")) parent = atoi(value);
            else if (!strcmp(arg, "--filter") && filter_count < MAX_FILTERS)
            {
                char *tab, *copy = strdup(value);
                if (!copy || !(tab = strchr(copy, '\t'))) { free(copy); continue; }
                *tab = 0;
                filters[filter_count].name = copy;
                filters[filter_count++].patterns = tab + 1;
            }
        }
    }
    if (!out_path)
    {
        fprintf(stderr, "usage: tuxoffice-file-dialog --out FILE [options]\n");
        return 2;
    }

#ifdef SYS_pidfd_open
    if (parent > 0) parent_fd = syscall(SYS_pidfd_open, parent, 0);
#endif
    {
        struct sigaction action = { .sa_handler = on_signal };
        sigaction(SIGTERM, &action, NULL);
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGHUP, &action, NULL);
    }
    if ((r = sd_bus_open_user(&bus)) < 0) return fail("no session bus");
    if ((r = sd_bus_get_unique_name(bus, &unique)) < 0) return fail("no bus name");

    /* Subscribe to the request's response before making the call (see the portal documentation). */
    snprintf(token, sizeof(token), "tuxoffice%d", (int)getpid());
    if (!(sender = strdup(unique + 1))) return fail("out of memory");
    for (char *p = sender; *p; p++) if (*p == '.') *p = '_';
    if (asprintf(&request_path, "/org/freedesktop/portal/desktop/request/%s/%s", sender, token) < 0)
        return fail("out of memory");
    if ((r = sd_bus_match_signal(bus, NULL, "org.freedesktop.portal.Desktop", request_path,
                                 "org.freedesktop.portal.Request", "Response", on_response, &response)) < 0)
        return fail("cannot subscribe to the portal response");

    if ((r = sd_bus_message_new_method_call(bus, &m, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                            "org.freedesktop.portal.FileChooser", save ? "SaveFile" : "OpenFile")) < 0)
        return fail("cannot create the portal call");
    sd_bus_message_append(m, "ss", "", title);
    sd_bus_message_open_container(m, 'a', "{sv}");
    sd_bus_message_append(m, "{sv}", "handle_token", "s", token);
    sd_bus_message_append(m, "{sv}", "modal", "b", 1);
    if (accept) sd_bus_message_append(m, "{sv}", "accept_label", "s", accept);
    if (save && name) sd_bus_message_append(m, "{sv}", "current_name", "s", name);
    if (multiple && !save) sd_bus_message_append(m, "{sv}", "multiple", "b", 1);
    if (directory && !save) sd_bus_message_append(m, "{sv}", "directory", "b", 1);
    if ((folder = linux_folder(folder)))
    {
        /* current_folder is a NUL-terminated byte array. */
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "current_folder");
        sd_bus_message_open_container(m, 'v', "ay");
        sd_bus_message_append_array(m, 'y', folder, strlen(folder) + 1);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
    }
    if (filter_count && !directory)
    {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "filters");
        sd_bus_message_open_container(m, 'v', "a(sa(us))");
        sd_bus_message_open_container(m, 'a', "(sa(us))");
        for (i = 0; i < filter_count; i++) append_filter(m, i);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        if (filter_index >= 0 && filter_index < filter_count)
        {
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "current_filter");
            sd_bus_message_open_container(m, 'v', "(sa(us))");
            append_filter(m, filter_index);
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);
        }
    }
    if ((r = sd_bus_message_close_container(m)) < 0) return fail("cannot build the portal call");

    /* Wait as long as the user takes. */
    if ((r = sd_bus_call(bus, m, UINT64_MAX, &error, &reply)) < 0)
        return fail(error.message ? error.message : "the file chooser portal is not available");
    if ((r = sd_bus_message_read(reply, "o", &handle)) < 0) return fail("bad portal reply");
    if (strcmp(handle, request_path))
    {
        /* Old portals choose the request path themselves. */
        if ((r = sd_bus_match_signal(bus, NULL, "org.freedesktop.portal.Desktop", handle,
                                     "org.freedesktop.portal.Request", "Response", on_response, &response)) < 0)
            return fail("cannot subscribe to the portal response");
    }

    while (!response.done)
    {
        if ((r = sd_bus_process(bus, NULL)) < 0) return fail("lost the session bus");
        if (r == 0 && (r = sd_bus_wait(bus, 500000)) < 0 && r != -EINTR) return fail("lost the session bus");
        /* The program that asked for the file is gone (crashed or killed),
         * or this helper is asked to stop: close the dialog, which the
         * portal would otherwise leave on the desktop. */
        if (stopping || parent_gone(parent, parent_fd))
        {
            sd_bus_call_method(bus, "org.freedesktop.portal.Desktop", handle, "org.freedesktop.portal.Request",
                               "Close", NULL, NULL, "");
            return 1;
        }
    }

    if (response.code == 0 && response.path_count)
    {
        if (response.filter < 0) response.filter = filter_index;
        return write_result("ok", &response);
    }
    return write_result(response.code == 1 || response.code == 0 ? "cancel" : "error the dialog failed", &response);
}
