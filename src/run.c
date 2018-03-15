
/* Copyright (c) 2010-2017 the corto developers
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <corto/corto.h>
#include <corto/argparse/argparse.h>
#include <sys/stat.h>
#include <errno.h>

static int retcode;

typedef struct corto_fileMonitor {
    char *file;
    char *lib;
    time_t mtime;
} corto_fileMonitor;

static
corto_fileMonitor* corto_monitor_new(
    const char *file,
    const char *lib)
{
    corto_fileMonitor *mon = corto_alloc(sizeof(corto_fileMonitor));
    mon->file = corto_strdup(file);
    mon->lib = lib ? corto_strdup(lib) : NULL;
    mon->mtime = 0;

    return mon;
}

static
void corto_monitor_free(
    corto_fileMonitor *mon)
{
    free(mon->file);
    free(mon->lib);
    free(mon);
}

static
void corto_add_changed(
    corto_ll *libs,
    corto_string lib)
{
    if (*libs && lib) {
        corto_iter iter = corto_ll_iter(*libs);
        while (corto_iter_hasNext(&iter)) {
            corto_string l = corto_iter_next(&iter);
            if (!strcmp(l, lib)) {
                return;
            }
        }
    }

    if (!*libs) {
        *libs = corto_ll_new();
    }
    corto_ll_append(*libs, lib);
}

static
corto_ll corto_get_modified(
    corto_ll files,
    corto_ll changed)
{
    corto_int32 i = 0;
    corto_ll libs = NULL;

    if (changed) {
        corto_ll_free(changed);
        changed = NULL;
    }

    if (files) {
        corto_iter iter = corto_ll_iter(files);
        while (corto_iter_hasNext(&iter)) {
            struct stat attr;
            corto_fileMonitor *mon = corto_iter_next(&iter);

            if (stat(mon->file, &attr) < 0) {
                corto_error("failed to stat '%s' (%s)\n",
                mon->file, strerror(errno));
            }

            if (mon->mtime) {
                if (mon->mtime != attr.st_mtime) {
                    corto_info("detected change in file '%s' (%d vs %d)", mon->file, mon->mtime, attr.st_mtime);
                    corto_add_changed(&libs, mon->lib);
                } else {
                    corto_debug("no change in file '%s'", mon->file);
                }
            } else {
                corto_debug("ignoring file '%s'", mon->file);
            }
            mon->mtime = attr.st_mtime;

            i++;
        }
    }

    return libs;
}

static
int compare_monitor(
    void *o1,
    void *o2)
{
    corto_fileMonitor
        *m1 = o1,
        *m2 = o2;

    if (!strcmp(m1->file, m2->file)) {
        return 0;
    }
    return 1;
}

static
bool filter_file(
    const char *file)
{
    char *ext = strchr(file, '.');

    if (!corto_isdir(file) &&
        strncmp(file, "test", 4) &&
        strncmp(file, "bin", 3) &&
        strcmp(file, "include/_type.h") &&
        strcmp(file, "include/_load.h") &&
        strcmp(file, "include/_interface.h") &&
        strcmp(file, "include/_api.h") &&
        strcmp(file, "include/_project.h") &&
        strcmp(file, "include/.prefix") &&
        !(file[0] == '.') &&
        ext &&
        strcmp(ext, ".so"))
    {
        return true;
    }

    return false;
}

static
corto_ll corto_gather_files(
    const char *project_dir,
    const char *app_bin,
    corto_ll old_files,
    bool *files_removed)
{
    corto_ll result = corto_ll_new();
    corto_iter it;

    if (corto_dir_iter(project_dir, "//", &it))  {
        goto error;
    }

    /* Find files that are not binaries, directories or build artefacts */
    while (corto_iter_hasNext(&it)) {
        char *file = corto_iter_next(&it);
        if (filter_file(file)) {
            corto_fileMonitor *m = corto_monitor_new(
                strarg("%s/%s", project_dir, file), app_bin);
            corto_ll_append(result, m);
        }
    }

    /* Update new filelist with timestamps from old list */
    if (old_files) {
        corto_fileMonitor *m;
        while ((m = corto_ll_takeFirst(old_files))) {
            corto_fileMonitor *m_new = corto_ll_find(
                result, compare_monitor, m);
            if (m_new) {
                m_new->mtime = m->mtime;
            } else {
                if (files_removed) {
                    *files_removed = true;
                }
            }
            corto_monitor_free(m);
        }

        corto_ll_free(old_files);
    }

    return result;
error:
    corto_ll_free(result);
    return NULL;
}

static
corto_ll corto_wait_for_changes(
    corto_proc pid,
    const char *project_dir,
    const char *app_bin,
    corto_ll changed)
{
    corto_int32 i = 0;

    /* Collect initial set of files in project directory */
    corto_ll files = corto_gather_files(project_dir, app_bin, NULL, NULL);

    /* Get initial timestamps for all files */
    corto_get_modified(files, NULL);

    if (changed) {
        corto_ll_free(changed);
        changed = NULL;
    }

    do {
        corto_sleep(0, 50000000);

        /* Check if process is still running every 50ms */
        if (pid) {
            if ((retcode = corto_proc_check(pid, NULL))) {
                break;
            }
        }

        i++;

        /* Check files every second */
        if (!(i % 20)) {
            bool files_removed = false;

            /* Refresh files, new files will show up with timestamp 0 */
            files = corto_gather_files(
                project_dir, app_bin, files, &files_removed);

            /* Check for changes, new files will show up as changed */
            changed = corto_get_modified(files, changed);

            /* If files have been removed but no files have changed, create an
             * empty list, so it will still trigger a rebuild */
            if (!changed && files_removed) {
                changed = corto_ll_new();
            }
        } else {
            continue;
        }
    }while (!changed || (pid && retcode));

    return changed;
}

static
int corto_build_project(
    const char *path)
{
    corto_int8 procResult = 0;

    /* Build */
    corto_proc pid = corto_proc_run("bake", (char*[]){
        "bake",
        (char*)path,
         NULL
    });

    if (corto_proc_wait(pid, &procResult) || procResult) {
        corto_throw("failed to build '%s'", path);

        /* Ensure that the binary folder is gone */
        corto_rm(strarg("%s/bin", path));
        goto error;
    }

    return 0;
error:
    return -1;
}

static
corto_proc corto_run_exec(
    const char *exec,
    bool is_package,
    char *local_argv[])
{
    if (!is_package) {
        return corto_proc_run(exec, local_argv);
    } else {
        char *argv[256];

        /* Copy arguments into vector */
        argv[0] = "corto";
        argv[1] = "--keep-alive";
        argv[2] = "-l";
        argv[3] = (char*)exec;

        int i;
        for (i = 0; (local_argv[i + 1] != NULL); i ++) {
            argv[i + 4] = local_argv[i + 1];
        }
        argv[i + 4] = NULL;

        return corto_proc_run("corto", argv);
    }
}

static
int16_t corto_run_interactive(
    const char *project_dir,
    const char *app_bin,
    bool is_package,
    char *argv[])
{
    corto_proc pid = 0;
    corto_ll changed = NULL;
    corto_uint32 retries = 0;
    corto_int32 rebuild = 0;

    /* Add $HOME/.corto/lib to LD_LIBRARY_PATH so that 3rd party libraries
     * installed to /usr/local/lib are also available when doing development
     * in local environment. */
    corto_setenv("LD_LIBRARY_PATH", "$HOME/.corto/lib:$LD_LIBRARY_PATH");

    while (true) {

        if (!retries || changed) {
            /* Build the project */
            corto_build_project(project_dir);
            rebuild++;
        }

        if (pid && rebuild) {
            /* Send interrupt signal to process */
            if (corto_proc_kill(pid, CORTO_SIGINT)) {
                /* Wait until process has exited */
                corto_proc_wait(pid, NULL);
            }
            rebuild = 0;
            pid = 0;
        }

        /* Test whether the app exists, then start it */
        if (corto_file_test(app_bin)) {
            if (retries && !pid) {
                corto_info("restarting app (%dx)", retries);
            }

            if (!pid) {
                /* Set CORTO_CONFIG while loading process */
                corto_setenv("CORTO_CONFIG", "%s/config", project_dir);

                /* Run process, ensure proc name is first argument */
                {
                    char *local_argv[1024] = {(char*)app_bin};
                    int i = 1;
                    while (argv[i]) {
                        local_argv[i] = argv[i];
                        i ++;
                    }

                    pid = corto_run_exec(app_bin, is_package, local_argv);
                }

                /* Unset CORTO_CONFIG so it doesn't mess up the build */
                corto_setenv("CORTO_CONFIG", NULL);
            }

            /* Wait until either source changes, or executable finishes */
            changed = corto_wait_for_changes(
                pid, project_dir, app_bin, changed);

            /* Set pid to 0 if process has exited */
            if (retcode) {
                pid = 0;
            }
        } else {
            corto_error(
                "build failed! (press Ctrl-C to exit"
                " or change files to rebuild)\n");

            /* Wait for changed before trying again */
            changed = corto_wait_for_changes(
                0, project_dir, app_bin, changed);
        }

        /* If the process segfaults, wait for changes and rebuild */
        if (retcode) {
            if ((retcode == 11) || (retcode == 6)) {
                corto_error("segmentation fault, fix your code!");
            } else {
                corto_info(
                    "process has exited! (press Ctrl-C to exit"
                    " or change files to restart)");
            }

            changed = corto_wait_for_changes(
                0, project_dir, app_bin, changed);
            retcode = 0;
            pid = 0;
        }

        retries++;
    }

    if (retcode != -1) {
        corto_error("process stopped with error (%d)", retcode);
    }

    return 0;
}

static
bool corto_is_valid_project(
    const char *project)
{
    /* Test if directory exists */
    if (corto_file_test(project) && corto_isdir(project)) {
        /* Test if specified directory has project.json */
        if (corto_file_test(strarg("%s/project.json", project))) {
            /* Valid corto project found */
            return true;
        }
    }

    return false;
}

static
const char *corto_name_from_id(
    const char *id)
{
    const char *result = strrchr(id, '/');
    if (result) {
        result ++;
    } else {
        result = id;
    }
    return result;
}

static
void corto_str_strip(
    char *str)
{
    int len = strlen(str);
    char *ptr = &str[len - 1];

    while (isspace(*ptr)) {
        *ptr = '\0';
        ptr --;
    }
}

int cortomain(int argc, char *argv[]) {
    char *config = "debug"; /* Configuration used for building */
    char *app_id = NULL; /* Full id of application */
    const char *app_name = NULL; /* Last element of application id */
    const char *app_bin = NULL; /* Application executable */
    bool is_package = false;
    const char *package_path = NULL; /* App location in public package repository */
    char *project_dir = NULL; /* Project directory containing sources */

    corto_ll interactive, dir;

    corto_argdata *data = corto_argparse(
      argv,
      (corto_argdata[]){
        {"$0", NULL, NULL}, /* Ignore 'run' */
        {"--interactive", &interactive, NULL},
        {"$1", &dir, NULL},
        {"*", NULL, NULL},
        {NULL}
      }
    );

    if (!data) {
        corto_throw("invalid arguments");
        goto error;
    }

    if (dir && strcmp((app_id = corto_ll_get(dir, 0)), ".")) {
        app_name = corto_name_from_id(app_id);

        /* First check if passed argument is a valid directory */
        if (corto_is_valid_project(app_id)) {
            project_dir = app_id;
        }

        /* If project is not found, lookup in package repositories */
        if (!project_dir) {
            package_path = corto_locate(app_id, NULL, CORTO_LOCATE_PACKAGE);
            if (!package_path) {
                corto_throw("failed to find application '%s'", app_id);
                goto error;
            }

            /* Check if the package repository contains a link back to the
             * location of the project. */
            char *source_file = corto_asprintf("%s/source.txt", package_path);
            if (corto_file_test(source_file)) {
                project_dir = corto_file_load(source_file);
                corto_str_strip(project_dir); /* Remove trailing whitespace */

                if (!corto_is_valid_project(project_dir)) {
                    /* Directory pointed to is not a valid corto project. Don't
                     * give up yet, maybe the project got moved. */
                    corto_warning(
                        "package '%s' points to project folder '%s'"
                        " which does not contain a valid corto project",
                        app_id, project_dir);
                    project_dir = NULL;
                }
            }
        }
        /* Test if current directory is a valid project */
    } else if (corto_is_valid_project(".")) {
        project_dir = ".";
    } else {
        corto_throw("current directory is not a valid corto project");
        goto error;
    }

    if (project_dir) {
        /* Read project.json to get application id */
        corto_object pkg = NULL;
        char *json = corto_file_load(strarg("%s/project.json", project_dir));
        if (corto_deserialize(&pkg, "text/json", json)) {
            corto_throw("failed to parse 'project.json'");
            goto error;
        }
        if (!pkg) {
            corto_throw("failed to deserialize '%s'", json);
            goto error;
        }

        app_id = corto_strdup(corto_fullpath(NULL, pkg));
        app_name = corto_name_from_id(app_id);

        if (!strcmp(corto_idof(corto_typeof(pkg)), "package")) {
            is_package = true;
        }

        corto_delete(pkg);
        free(json);

        /* If project is found, point to executable in project bin */
        if (!is_package) {
            app_bin = corto_asprintf(
                "%s/bin/%s-%s/%s",
                project_dir,
                CORTO_PLATFORM_STRING,
                config,
                app_name);
        } else {
            app_bin = corto_asprintf(
                "%s/bin/%s-%s/lib%s.so",
                project_dir,
                CORTO_PLATFORM_STRING,
                config,
                app_name);
        }
    } else {
        /* If project directory is not found, locate the binary in the
         * package repository. This only allows for running the
         * application, not for interactive building */
        app_bin = corto_locate(app_id, NULL, CORTO_LOCATE_BIN);
        if (!app_bin) {
            /* We have no project dir and no executable. No idea how to
             * build this project! */
            corto_throw("executable not found for '%s'", app_id);
            goto error;
        }

        if (corto_locate(app_id, NULL, CORTO_LOCATE_LIB)) {
            is_package = true;
        }
    }

    if (interactive) {
        if (!project_dir) {
            corto_warning(
              "don't know location of sourcefiles, interactive mode disabled");
            interactive = NULL;
        }
    }

    corto_info("starting app '%s'", app_id);
    corto_info("  executable = '%s'", app_bin);
    corto_info("  project path = '%s'", project_dir);
    corto_info("  project kind = '%s'", is_package ? "package" : "application");
    corto_info("  interactive = '%s'", interactive ? "true" : "false");

    if (interactive) {
        /* Run process & monitor source for changes */
        if (corto_run_interactive(project_dir, app_bin, is_package, &argv[1]))
        {
            goto error;
        }
    } else {
        /* Just run process */
        corto_proc pid;
        corto_trace("starting process '%s'", app_bin);

        /* Set CORTO_CONFIG to process configuration directory */
        corto_setenv("CORTO_CONFIG", "%s/config", project_dir);

        if (argc > 1) {
            pid = corto_run_exec(app_bin, is_package, &argv[1]);
        } else {
            pid = corto_run_exec(
                app_bin, is_package, (char*[]){(char*)app_bin, NULL});
        }
        if (!pid) {
            corto_throw("failed to start process '%s'", app_bin);
            goto error;
        }

        corto_ok("waiting for process '%s'", argv[1]);
        corto_int8 result = 0, sig = 0;
        if ((sig = corto_proc_wait(pid, &result)) || result) {
            if (sig > 0) {
                corto_throw("process crashed (%d)", sig);
                goto error;
            } else {
                corto_throw("process returned %d", result);
                goto error;
            }
        }
    }

    corto_argclean(data);

    return 0;
error:
    return -1;
}
