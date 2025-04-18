/*
 * client.c - Command-line client for controlling alink_manager
 *
 * Compile with:
 *     gcc -o client client.c
 *
 * Usage:
 *   client [--verbose] <server_ip> "<command>"
 *   client --help
 *
 * Options:
 *   -v, --verbose   Enable debug output
 *   -h, --help      Show this help message
 *
 * Commands supported by the server:
 *   start_alink
 *       Start alink_drone on the drone.
 *
 *   stop_alink
 *       Stop alink_drone (killall alink_drone).
 *
 *   restart_majestic
 *       Restart the majestic process on the drone (killall -HUP majestic).
 *
 *   change_channel <n>
 *       Change the drone's WiFi channel to <n>. Requires ground-station confirmation.
 *
 *   confirm_channel_change
 *       Confirm a pending channel change (sent automatically by this client).
 *
 *   set_video_mode <size> <fps> <exposure> <crop>
 *       Atomically set video size, frame rate, exposure, and crop.
 *       <crop> must be quoted if it contains spaces, e.g. "100 200 300 400".
 *
 *   stop_msposd
 *       Stop msposd process.
 *
 *   start_msposd
 *       Start msposd process.
 *
 *   adjust_txprofiles
 *       Update /etc/txprofiles.conf and restart alink_drone.
 *
 *   adjust_alink
 *       Update /etc/alink.conf and restart alink_drone.
 *
 *   info
 *       Retrieve current configuration and status from the drone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

#define PORT 12355
#define BUF_SIZE 1024

static int verbose = 0;

// Print usage and command descriptions
void print_help(const char *prog) {
    printf(
        "Usage:\n"
        "  %s [--verbose] <server_ip> \"<command>\"\n"
        "  %s --help\n\n"
        "Options:\n"
        "  -v, --verbose   Enable debug output\n"
        "  -h, --help      Show this help message\n\n"
        "Commands supported by the server:\n"
        "  start_alink\n"
        "      Start alink_drone on the drone.\n\n"
        "  stop_alink\n"
        "      Stop alink_drone (killall alink_drone).\n\n"
        "  restart_majestic\n"
        "      Restart the majestic process on the drone (killall -HUP majestic).\n\n"
        "  change_channel <n>\n"
        "      Change the drone's WiFi channel to <n>.\n"
        "      Requires ground-station confirmation.\n\n"
        "  confirm_channel_change\n"
        "      Confirm a pending channel change.\n\n"
        "  set_video_mode <size> <fps> <exposure> <crop>\n"
        "      Atomically set video size, frame rate, exposure, and crop.\n"
        "      <crop> must be quoted if it contains spaces, e.g. \"100 200 300 400\".\n\n"
        "  stop_msposd\n"
        "      Stop the msposd process.\n\n"
        "  start_msposd\n"
        "      Start the msposd process.\n\n"
        "  adjust_txprofiles\n"
        "      Update /etc/txprofiles.conf and restart alink_drone.\n\n"
        "  adjust_alink\n"
        "      Update /etc/alink.conf and restart alink_drone.\n\n"
        "  info\n"
        "      Retrieve current configuration and status from the drone.\n",
        prog, prog
    );
}

#define CONNECT_TIMEOUT   2   // seconds to wait per connect()
#define MAX_CONNECT_TRIES 3   // how many times to retry

// Attempts a non-blocking connect with a timeout.
// Returns 0 on success, -1 on failure (errno set appropriately).
static int connect_with_timeout(int sock, 
                                const struct sockaddr_in *addr, 
                                socklen_t addrlen, 
                                int timeout_secs)
{
    int flags, res, err;
    socklen_t errlen;

    // 1) make socket non-blocking
    if ((flags = fcntl(sock, F_GETFL, 0)) < 0) return -1;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    // 2) start connect()
    res = connect(sock, (const struct sockaddr*)addr, addrlen);
    if (res < 0 && errno == EINPROGRESS) {
        // 3) wait for it to become writable
        fd_set wfds;
        struct timeval tv = { timeout_secs, 0 };
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        res = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (res <= 0) {
            // timeout or select error
            errno = (res == 0 ? ETIMEDOUT : errno);
            goto fail;
        }
        // 4) check actual connect() result
        err = 0; errlen = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
            goto fail;
        }
        if (err) {
            errno = err;
            goto fail;
        }
    }
    else if (res < 0) {
        // immediate error other than EINPROGRESS
        goto fail;
    }

    // 5) restore flags
    if (fcntl(sock, F_SETFL, flags) < 0) return -1;
    return 0;

fail:
    // restore blocking just in case
    fcntl(sock, F_SETFL, flags);
    return -1;
}

int send_command_get_response(const char *server_ip,
                              const char *command,
                              char *response,
                              size_t resp_size)
{
    int sock, attempt;
    struct sockaddr_in server_addr;
    struct timeval tv;

    // Prepare address struct once
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(PORT);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return -1;
    }

    for (attempt = 1; attempt <= MAX_CONNECT_TRIES; ++attempt) {
        if (verbose) printf("[DEBUG] Creating socket (try %d)...\n", attempt);
        if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
            perror("Socket creation error");
            return -1;
        }

        // 2s recv timeout (unchanged)
        tv.tv_sec  = CONNECT_TIMEOUT;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (verbose) printf("[DEBUG] Connecting to %s:%d (try %d)...\n",
                            server_ip, PORT, attempt);

        if (connect_with_timeout(sock, &server_addr, sizeof(server_addr),
                                 CONNECT_TIMEOUT) == 0) {
            // success!
            break;
        }

        perror("[DEBUG] connect() failed");
        close(sock);

        if (attempt == MAX_CONNECT_TRIES) {
            fprintf(stderr, "Failed to connect after %d attempts\n",
                    MAX_CONNECT_TRIES);
            return -1;
        }

        // small back‑off before retrying
        sleep(1);
    }

    // At this point `sock` is connected.
    if (verbose) printf("[DEBUG] Sending command: %s\n", command);
    if (send(sock, command, strlen(command), 0) < 0) {
        perror("Send failed");
        close(sock);
        return -1;
    }

    int n = read(sock, response, resp_size - 1);
    if (n > 0) {
        response[n] = '\0';
    } else {
        snprintf(response, resp_size, "No immediate rejection.  Moving on...");
    }
    if (verbose) printf("[DEBUG] Received: %s\n", response);
    close(sock);
    return 0;
}


// Read NIC list from /etc/default/wifibroadcast
char **get_nics(int *count) {
    FILE *fp = fopen("/etc/default/wifibroadcast", "r");
    if (!fp) {
        if (verbose) printf("[DEBUG] Cannot open /etc/default/wifibroadcast\n");
        *count = 0;
        return NULL;
    }
    char line[256], nics_line[256] = "";
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "WFB_NICS=", 9) == 0) {
            strcpy(nics_line, strchr(line, '=') + 1);
            nics_line[strcspn(nics_line, "\r\n")] = 0;
            break;
        }
    }
    fclose(fp);
    if (verbose) printf("[DEBUG] Raw NICs line: %s\n", nics_line);
    // Strip outer quotes
    int len = strlen(nics_line);
    if (len >= 2 && nics_line[0]=='"' && nics_line[len-1]=='"') {
        memmove(nics_line, nics_line+1, len-2);
        nics_line[len-2] = '\0';
    }
    if (verbose) printf("[DEBUG] Processed NICs line: %s\n", nics_line);

    char *copy = strdup(nics_line), *tok = strtok(copy, " ");
    int alloc = 4, idx = 0;
    char **arr = malloc(alloc * sizeof(char*));
    while (tok) {
        if (idx >= alloc) {
            alloc *= 2;
            arr = realloc(arr, alloc * sizeof(char*));
        }
        arr[idx++] = strdup(tok);
        if (verbose) printf("[DEBUG] Found NIC: %s\n", tok);
        tok = strtok(NULL, " ");
    }
    free(copy);
    *count = idx;
    return arr;
}

// Set all NICs to given channel
void local_change_channel(int channel) {
    int cnt = 0;
    char **nics = get_nics(&cnt);
    if (!nics || cnt == 0) {
        if (verbose) printf("[DEBUG] No NICs to change\n");
        return;
    }
    for (int i = 0; i < cnt; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "iw dev %s set channel %d", nics[i], channel);
        if (verbose) printf("[DEBUG] %s\n", cmd);
        system(cmd);
        free(nics[i]);
    }
    free(nics);
}

// Helper function that attempts to update a given file.
// Returns 1 on success, 0 on failure.
int update_file(const char *filepath, const char *key, const char *format, int channel) {
    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open file: %s\n", filepath);
        return 0;
    }
    // Create a temporary file in the same directory
    char tmp_filepath[256];
    snprintf(tmp_filepath, sizeof(tmp_filepath), "%s.tmp", filepath);
    FILE *fp_tmp = fopen(tmp_filepath, "w");
    if (!fp_tmp) {
        fprintf(stderr, "Cannot open temporary file: %s for file %s\n", tmp_filepath, filepath);
        fclose(fp);
        return 0;
    }
    
    char line[1024];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (strstr(line, key) != NULL) {
            // Replace entire line with new channel value.
            char newline[1024];
            snprintf(newline, sizeof(newline), format, channel);
            fprintf(fp_tmp, "%s\n", newline);
        } else {
            fputs(line, fp_tmp);
        }
    }
    
    fclose(fp);
    fclose(fp_tmp);
    
    // Replace original file with the temporary file.
    if (rename(tmp_filepath, filepath) != 0) {
        perror("Error renaming temporary file");
        return 0;
    }
    return 1;
}

void save_new_channel_to_files(int channel) {
    const char *file1 = "/etc/wifibroadcast.cfg";
    const char *file2 = "/config/gs.conf";
    int success1, success2;

    // Update file1 unconditionally
    success1 = update_file(file1, "wifi_channel", "wifi_channel = '%d'", channel);
    
    // Update file2 only if it exists.
    if (access(file2, F_OK) == 0) {
        success2 = update_file(file2, "wifi_channel", "wifi_channel = '%d'", channel);
    } else {
        fprintf(stderr, "File %s does not exist, skipping update.\n", file2);
        success2 = 0;
    }
    
    // Report final status
    if (!success1 && !success2) {
        fprintf(stderr, "Warning: Could not write to either file.  Channel change will not persist after reboot!\n");
    } else if (success1) {
        fprintf(stderr, "Succesfully wrote new channel to %s\n", file1);
    } else if (success2) {
        fprintf(stderr, "Succesfully wrote new channel to %s\n", file2);
    }
}

int main(int argc, char *argv[]) {
    static struct option opts[] = {
        {"verbose", no_argument, 0, 'v'},
        {"help",    no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "vh", opts, NULL)) != -1) {
        if (opt == 'v') verbose = 1;
        else if (opt == 'h') { print_help(argv[0]); return 0; }
        else { print_help(argv[0]); return 1; }
    }
    if (optind + 2 > argc) {
        print_help(argv[0]);
        return 1;
    }

    const char *server_ip = argv[optind];
    const char *command   = argv[optind+1];
    char response[BUF_SIZE];

    // Handle change_channel specially (with confirmation & rollback)
    if (strncmp(command, "change_channel", 14) == 0) {
        int ch;
        if (sscanf(command, "change_channel %d", &ch) == 1) {
            // get original channel
            int cnt; 
            char **nics = get_nics(&cnt);
            char orig[16] = "unknown";
            if (cnt > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "iw dev %s info | grep channel | awk '{print $2}'",
                         nics[0]);
                FILE *p = popen(buf, "r");
                if (p && fgets(orig, sizeof(orig), p))
                    orig[strcspn(orig, "\r\n")] = 0;
                if (p) pclose(p);
            }
            if (nics) { for (int i = 0; i < cnt; i++) free(nics[i]); free(nics); }

            // send change
            send_command_get_response(server_ip, command, response, sizeof(response));
            printf("%s\n", response);
            sleep(1);

            if (!strstr(response, "Failed")) {
                local_change_channel(ch);
                // ping test
                int ok = 0;
                for (int i = 0; i < 5; i++) {
                    char ping[64];
                    snprintf(ping, sizeof(ping), "ping -c1 -W1 %s >/dev/null", server_ip);
                    if (!system(ping)) { ok = 1; break; }
                    sleep(1);
                }
                if (ok) {
                    send_command_get_response(server_ip, "confirm_channel_change", response, sizeof(response));
                    printf("%s\n", response);
                    // make persistent after reboot
                    save_new_channel_to_files(ch);
                } else {
                    printf("No contact; reverting to channel %s\n", orig);
                    local_change_channel(atoi(orig));
                }
            }
        } else {
            fprintf(stderr, "Invalid change_channel format\n");
        }
    }
    // Handle set_video_mode command
    else if (strncmp(command, "set_video_mode", 14) == 0) {
        char size[32], crop[128];
        int fps, exposure;
        // Expected format: set_video_mode <size> <fps> <exposure> '<crop>'
        if (sscanf(command, "set_video_mode %31s %d %d '%127[^']'", size, &fps, &exposure, crop) == 4) {
            // send it straight to server
            if (send_command_get_response(server_ip, command, response, sizeof(response)) == 0) {
                printf("%s\n", response);
            }
        } else {
            fprintf(stderr, "Invalid set_video_mode format\n");
        }
    }
    // Handle new msposd commands
    else if (strncmp(command, "stop_msposd", 11) == 0 ||
             strncmp(command, "start_msposd", 12) == 0) {
        if (send_command_get_response(server_ip, command, response, sizeof(response)) == 0)
            printf("%s\n", response);
    }
    // All remaining commands (start_alink, stop_alink, restart_majestic, adjust_txprofiles, adjust_alink, info, etc.)
    else {
        if (send_command_get_response(server_ip, command, response, sizeof(response)) == 0)
            printf("%s\n", response);
    }

    return 0;
}
