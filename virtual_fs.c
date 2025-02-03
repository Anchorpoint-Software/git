#include "virtual_fs.h"

#include "git-compat-util.h"
#include "thread-utils.h"
#include "run-command.h"
#include "strvec.h"
#include "strbuf.h"
#include "sigchain.h"
#include "hex.h"
#include "abspath.h"

static void normalize_directory_name(char *path) {
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '\\') {
        path[len - 1] = '\0';
    }
}

static int compare_versions(const void *a, const void *b) {
    const char *verA = *(const char **)a;
    const char *verB = *(const char **)b;

    int numA[3] = {0}, numB[3] = {0};
    if (sscanf(verA, "%d.%d.%d", &numA[0], &numA[1], &numA[2]) < 1) return -1;
    if (sscanf(verB, "%d.%d.%d", &numB[0], &numB[1], &numB[2]) < 1) return 1;

    for (int i = 0; i < 3; i++) {
        if (numA[i] < numB[i]) return -1;
        if (numA[i] > numB[i]) return 1;
    }
    return 0;
}

static char *get_install_folder(void) {
    static char installDirectory[1024] = "";
    char *installOverwrite = getenv("ANCHORPOINT_ROOT");
    if (installOverwrite) {
        strlcpy(installDirectory, installOverwrite, sizeof(installDirectory));
        // TODO: path must be /../Frameworks
        normalize_directory_name(installDirectory);
    } else if (strlen(installDirectory) == 0) {
        #ifndef GIT_WINDOWS_NATIVE
            strlcpy(installDirectory, "/Applications/Anchorpoint.app/Contents/Frameworks", sizeof(installDirectory));
        #else
            char anchorpointVersionsPath[1024];
            char *anchorpointVersions[256];
            char *localAppData = getenv("LOCALAPPDATA");
            int count = 0;
            struct dirent *entry;
            DIR *dir = NULL;

            if (!localAppData) {
                return NULL;
            }
            
            snprintf(anchorpointVersionsPath, sizeof(anchorpointVersionsPath), "%s\\Anchorpoint", localAppData);

            dir = opendir(anchorpointVersionsPath);
            if (!dir) return NULL;

            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_name[0] == '.' || strlen(entry->d_name) < 4 || strncmp(entry->d_name, "app-", 4) != 0) continue;
                anchorpointVersions[count++] = strdup(entry->d_name);
            }
            closedir(dir);

            if (count == 0) return NULL;

            qsort(anchorpointVersions, count, sizeof(char *), compare_versions);

            snprintf(installDirectory, sizeof(installDirectory), "%s\\%s", anchorpointVersionsPath, anchorpointVersions[count - 1]);
            normalize_directory_name(installDirectory);

            for (int i = 0; i < count; i++) free(anchorpointVersions[i]);
        #endif
    }
    return installDirectory;
}

static char *get_ap_cli_path(void) {
    static char cliPath[1024] = "";
    
    if (strlen(cliPath) == 0) {
        const char *installFolder = get_install_folder();
        if (!installFolder || strlen(installFolder) == 0) return NULL;
    
        #ifdef GIT_WINDOWS_NATIVE
            snprintf(cliPath, sizeof(cliPath), "%s\\ap.exe", installFolder);
        #else
            snprintf(cliPath, sizeof(cliPath), "%s/ap", installFolder);
        #endif
    }

    return cliPath;
}

struct ap_process {
    struct child_process cmd;
    int initialized;
    pthread_mutex_t mutex;
    FILE *in;
    FILE *out;
    const char *path;
};

static struct ap_process ap = {.cmd = CHILD_PROCESS_INIT, .initialized = 0, .in = NULL, .out = NULL, .path = NULL};

static void subprocess_exit_handler(struct child_process *process)
{
	sigchain_push(SIGPIPE, SIG_IGN);
	close(process->in);
	close(process->out);
	sigchain_pop(SIGPIPE);

    if (ap.in)
		fclose(ap.in);

    if (ap.out)
		fclose(ap.out);
}

void init_anchorpoint_mutex(void)
{
    init_recursive_mutex(&ap.mutex);
}

static int file_exists(const char *f)
{
	struct stat sb;
	return lstat(f, &sb) == 0;
}

int init_anchorpoint_process(void) 
{
    pthread_mutex_lock(&ap.mutex);
    if (ap.initialized == 1) {
        pthread_mutex_unlock(&ap.mutex);
        return 0;
    }

    ap.path = get_ap_cli_path();
    if (!file_exists(ap.path)) {
        return -1;
    }

    strvec_pushl(&ap.cmd.args, ap.path, "vfs", "connect", NULL);
	ap.cmd.in = -1;
	ap.cmd.out = -1;
    ap.cmd.err = -1;
    
    ap.cmd.use_shell = 0;
    ap.cmd.git_cmd = 0;
    ap.cmd.close_object_store = 0;
    ap.cmd.silent_exec_failure = 0;

    ap.cmd.clean_on_exit = 1;
	ap.cmd.clean_on_exit_handler = subprocess_exit_handler;

    if (start_command(&ap.cmd)) {
        die("Failed to run ap.exe process.");
    }

    ap.in = fdopen(ap.cmd.in, "w");
	if (!ap.in) {
        die("Failed to open file descriptor for writing to ap.exe.");
	}

	ap.out = fdopen(ap.cmd.out, "r");
	if (!ap.out) {
        die("Failed to open file descriptor for reading from ap.exe.");
	}

    ap.initialized = 1;
    pthread_mutex_unlock(&ap.mutex);
    return 0;
}

int is_path_virtual(const char* path) 
{
    int is_virtual = 0;
    struct strbuf line = STRBUF_INIT;
    if (!path) {
        die("is_path_virtual: path is NULL");
    }

    pthread_mutex_lock(&ap.mutex);

    if (!ap.initialized) {
        if (init_anchorpoint_process()) {
            die("is_path_virtual: ap.exe process not initialized");
        }
    }

    fprintf(ap.in, "virtual\n");
    fprintf(ap.in, "%s\n", absolute_path(path));
    fflush(ap.in);

    while (!strbuf_getline(&line, ap.out)) {
		if (!line.len)
			break;
		if (!strcmp(line.buf, "1")) {
			is_virtual = 1;
            break;
        }
        if (!strcmp(line.buf, "0")) {
			is_virtual = 0;
            break;
        }

        // error
        error("Failed to check if path is virtual: %s.", line.buf);
        break;
    }

    pthread_mutex_unlock(&ap.mutex);
    return is_virtual;
}

int create_placeholder(const char *path, unsigned int size, const struct object_id *oid) 
{
    int success = 0;
    struct strbuf line = STRBUF_INIT;
    if (!path) {
        die("create_placeholder: path is NULL");
    }
    if (!oid) {
        die("create_placeholder: oid is NULL");
    }

    pthread_mutex_lock(&ap.mutex);

    if (!ap.initialized) {
        if (init_anchorpoint_process()) {
            die("create_placeholder: ap.exe process not initialized");
        }
    }

    fprintf(ap.in, "placeholder\n");
    fprintf(ap.in, "%s\n", absolute_path(path));
    fprintf(ap.in, "%d\n", size);
    fprintf(ap.in, "%s\n", oid_to_hex(oid));
    fflush(ap.in);

    while (!strbuf_getline(&line, ap.out)) {
		if (!line.len)
			break;
		if (!strcmp(line.buf, "1")) {
			success = 1;
            break;
        }
        if (!strcmp(line.buf, "0")) {
			success = 0;
            break;
        }

        // error
        error("Failed to create placeholder: %s.", line.buf);
        break;
    }

    pthread_mutex_unlock(&ap.mutex);
    return success;
}

int is_sync_root(const char *path)
{
    int is_sync_root = 0;
    struct strbuf line = STRBUF_INIT;
    if (!path) {
        die("is_sync_root: path is NULL");
    }

    pthread_mutex_lock(&ap.mutex);

    if (!ap.initialized) {
        if (init_anchorpoint_process()) {
            return 0;
        }
    }

    fprintf(ap.in, "syncroot\n");
    fprintf(ap.in, "%s\n", absolute_path(path));
    fflush(ap.in);

    while (!strbuf_getline(&line, ap.out)) {
		if (!line.len)
			break;

		if (!strcmp(line.buf, "1")) {
			is_sync_root = 1;
            break;
        }
        if (!strcmp(line.buf, "0")) {
			is_sync_root = 0;
            break;
        }

        // error
        error("Failed to check if path is sync root: %s.", line.buf);
        break;
    }

    pthread_mutex_unlock(&ap.mutex);
    return is_sync_root;
}
