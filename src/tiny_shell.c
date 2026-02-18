#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <termios.h>
#include <sys/types.h>
#include <limits.h>

#define MAX_INPUT 4096
#define MAX_TOKENS 1024
#define MAX_ARGS 128
#define MAX_CMDS 64
#define MAX_JOBS 128
#define CMDLINE_LEN 1024

typedef enum { JOB_RUNNING, JOB_STOPPED, JOB_DONE } job_status_t;

typedef struct {
    int jid;                    
    pid_t pgid;                 
    char cmdline[CMDLINE_LEN]; 
    job_status_t status;       
    int is_background;          
} Job;

typedef struct {
    char *argv[MAX_ARGS];
    char *infile;
    char *outfile;
    int out_append;
    char *errfile;
} Command;

/* Globals for shell */
static Job jobs[MAX_JOBS];
static int next_jid = 1;
static pid_t shell_pgid;
static struct termios shell_tmodes;
static volatile sig_atomic_t sigchld_flag = 0; /* set by SIGCHLD handler */

/* Helper declarations */
static char *trim(char *s);
static void space_operators(const char *in, char *out, size_t outsz);
static int tokenize(char *buf, char *tokens[], int max_tokens);
static int parse_pipeline(char *tokens[], int ntokens, Command cmds[], int max_cmds);
static int handle_builtins(Command *c);
static void execute_single(Command *c, int foreground);
static void execute_pipeline(Command cmds[], int num_cmds, int foreground, int background_flag, char *orig_line);
static void sigchld_handler(int sig);
static void check_sigchld_and_reap(void);

/* Job table helpers */
static int job_add(pid_t pgid, const char *cmdline, int bg) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].jid == 0) {
            jobs[i].jid = next_jid++;
            jobs[i].pgid = pgid;
            jobs[i].status = JOB_RUNNING;
            jobs[i].is_background = bg;
            strncpy(jobs[i].cmdline, cmdline, CMDLINE_LEN-1);
            jobs[i].cmdline[CMDLINE_LEN-1] = '\0';
            return jobs[i].jid;
        }
    }
    return -1; /* no slot */
}

static Job* job_by_jid(int jid) {
    for (int i=0;i<MAX_JOBS;++i) if (jobs[i].jid == jid) return &jobs[i];
    return NULL;
}

static Job* job_by_pgid(pid_t pgid) {
    for (int i=0;i<MAX_JOBS;++i) if (jobs[i].jid != 0 && jobs[i].pgid == pgid) return &jobs[i];
    return NULL;
}

static void job_mark_done(pid_t pgid) {
    Job *j = job_by_pgid(pgid);
    if (j) { j->status = JOB_DONE; j->is_background = 0; }
}

static void job_mark_stopped(pid_t pgid) {
    Job *j = job_by_pgid(pgid);
    if (j) { j->status = JOB_STOPPED; }
}

static void job_mark_running(pid_t pgid) {
    Job *j = job_by_pgid(pgid);
    if (j) { j->status = JOB_RUNNING; }
}

static void job_remove_jid(int jid) {
    Job *j = job_by_jid(jid);
    if (!j) return;
    j->jid = 0;
    j->pgid = 0;
    j->cmdline[0] = '\0';
    j->status = JOB_DONE;
    j->is_background = 0;
}

static void print_job_status(Job *j) {
    if (!j) return;
    const char *stat = (j->status == JOB_RUNNING) ? "Running" : (j->status == JOB_STOPPED) ? "Stopped" : "Done";
    printf("[%d] %d %s\t%s\n", j->jid, j->pgid, stat, j->cmdline);
}

static void sigchld_handler(int sig) {
    (void)sig;
    sigchld_flag = 1;
}

static void check_sigchld_and_reap(void) {
    if (!sigchld_flag) return;
    sigchld_flag = 0;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        pid_t pgid = getpgid(pid);
        if (pgid < 0) pgid = pid; /* fallback */

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            /* terminated */
            job_mark_done(pgid);
            Job *j = job_by_pgid(pgid);
            if (j && j->is_background) {
                printf("\n[%d]+ Done\t%s\n", j->jid, j->cmdline);
            }
        } else if (WIFSTOPPED(status)) {
            job_mark_stopped(pgid);
            Job *j = job_by_pgid(pgid);
            if (j) {
                printf("\n[%d]+ Stopped\t%s\n", j->jid, j->cmdline);
            }
        } else if (WIFCONTINUED(status)) {
            job_mark_running(pgid);
            /* maybe notify */
        }
    }
}

static char *trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return s;
}

static void space_operators(const char *in, char *out, size_t outsz) {
    size_t inlen = strlen(in);
    size_t j = 0;
    for (size_t i=0;i<inlen && j+1<outsz;i++) {
        if (in[i] == '|' || in[i] == '<') {
            if (j+3 >= outsz) break;
            out[j++] = ' ';
            out[j++] = in[i];
            out[j++] = ' ';
        } else if (in[i] == '>') {
            if (i+1<inlen && in[i+1] == '>') {
                if (j+4>=outsz) break;
                out[j++]=' '; out[j++]='>'; out[j++]='>'; out[j++]=' ';
                i++;
            } else {
                if (j+3>=outsz) break;
                out[j++]=' '; out[j++]='>'; out[j++]=' ';
            }
        } else if (in[i]=='2' && i+1<inlen && in[i+1]=='>') {
            if (j+4>=outsz) break;
            out[j++]=' '; out[j++]='2'; out[j++]='>'; out[j++]=' ';
            i++;
        } else {
            out[j++] = in[i];
        }
    }
    out[j] = '\0';
}

/* Tokenize by whitespace */
static int tokenize(char *buf, char *tokens[], int max_tokens) {
    int n=0;
    char *saveptr=NULL;
    char *t = strtok_r(buf, " \t", &saveptr);
    while (t && n < max_tokens) {
        tokens[n++] = t;
        t = strtok_r(NULL, " \t", &saveptr);
    }
    return n;
}

/* Parse tokens into array of Command structs */
static int parse_pipeline(char *tokens[], int ntokens, Command cmds[], int max_cmds) {
    int cmd_idx = 0;
    int arg_idx = 0;
    if (ntokens == 0) return 0;

    memset(&cmds[cmd_idx], 0, sizeof(Command));
    for (int i=0;i<ntokens;i++) {
        char *t = tokens[i];
        if (strcmp(t, "|") == 0) {
            cmds[cmd_idx].argv[arg_idx] = NULL;
            cmd_idx++;
            if (cmd_idx >= max_cmds) { fprintf(stderr,"too many pipeline stages\n"); return -1; }
            memset(&cmds[cmd_idx], 0, sizeof(Command));
            arg_idx = 0;
            continue;
        }
        if (strcmp(t, "<") == 0) {
            if (i+1>=ntokens) { fprintf(stderr,"syntax error: < without file\n"); return -1; }
            cmds[cmd_idx].infile = tokens[++i];
            continue;
        }
        if (strcmp(t, ">") == 0) {
            if (i+1>=ntokens) { fprintf(stderr,"syntax error: > without file\n"); return -1; }
            cmds[cmd_idx].outfile = tokens[++i];
            cmds[cmd_idx].out_append = 0;
            continue;
        }
        if (strcmp(t, ">>") == 0) {
            if (i+1>=ntokens) { fprintf(stderr,"syntax error: >> without file\n"); return -1; }
            cmds[cmd_idx].outfile = tokens[++i];
            cmds[cmd_idx].out_append = 1;
            continue;
        }
        if (strcmp(t, "2>") == 0) {
            if (i+1>=ntokens) { fprintf(stderr,"syntax error: 2> without file\n"); return -1; }
            cmds[cmd_idx].errfile = tokens[++i];
            continue;
        }
        /* normal arg */
        if (arg_idx >= MAX_ARGS-1) { fprintf(stderr,"too many args\n"); return -1; }
        cmds[cmd_idx].argv[arg_idx++] = t;
    }
    cmds[cmd_idx].argv[arg_idx] = NULL;
    return cmd_idx + 1;
}

/* Handle built-ins for single command. Return 1 if handled (no fork), 0 otherwise. */
static int handle_builtins(Command *c) {
    if (!c->argv[0]) return 0;
    if (strcmp(c->argv[0], "exit") == 0) {
        exit(0);
    }
    if (strcmp(c->argv[0], "cd") == 0) {
        if (!c->argv[1]) { fprintf(stderr,"cd: missing operand\n"); return 1; }
        if (chdir(c->argv[1]) != 0) perror("cd");
        return 1;
    }
    if (strcmp(c->argv[0], "pwd") == 0) {
        char buf[4096];
        if (getcwd(buf,sizeof(buf))) printf("%s\n", buf);
        else perror("pwd");
        return 1;
    }
    if (strcmp(c->argv[0], "find") == 0) {
        if (!c->argv[1]) { fprintf(stderr,"find: missing filename\n"); return 1; }
        char cmd[4096];
        snprintf(cmd,sizeof(cmd),"find . -name \"%s\"", c->argv[1]);
        system(cmd);
        return 1;
    }

    /* fg and bg builtins: need job table */
    if (strcmp(c->argv[0], "jobs") == 0) {
        for (int i=0;i<MAX_JOBS;++i) if (jobs[i].jid) print_job_status(&jobs[i]);
        return 1;
    }
    if (strcmp(c->argv[0], "bg") == 0) {
        if (!c->argv[1]) { fprintf(stderr,"bg: missing job spec\n"); return 1; }
        char *s = c->argv[1];
        if (s[0] != '%') { fprintf(stderr,"bg: job spec should be %%N\n"); return 1; }
        int jid = atoi(s+1);
        Job *j = job_by_jid(jid);
        if (!j) { fprintf(stderr,"bg: no such job %d\n", jid); return 1; }
        if (kill(-j->pgid, SIGCONT) < 0) perror("kill (SIGCONT)");
        j->status = JOB_RUNNING;
        j->is_background = 1;
        printf("[%d]+ %s &\n", j->jid, j->cmdline);
        return 1;
    }
    if (strcmp(c->argv[0], "fg") == 0) {
        if (!c->argv[1]) { fprintf(stderr,"fg: missing job spec\n"); return 1; }
        char *s = c->argv[1];
        if (s[0] != '%') { fprintf(stderr,"fg: job spec should be %%N\n"); return 1; }
        int jid = atoi(s+1);
        Job *j = job_by_jid(jid);
        if (!j) { fprintf(stderr,"fg: no such job %d\n", jid); return 1; }

        /* bring to foreground */
        /* send SIGCONT */
        if (kill(-j->pgid, SIGCONT) < 0) perror("kill (SIGCONT)");
        j->status = JOB_RUNNING;
        j->is_background = 0;

        /* give terminal control to job pgid */
        if (tcsetpgrp(STDIN_FILENO, j->pgid) < 0) perror("tcsetpgrp fg");

        /* wait for job to finish or stop */
        int status;
        while (1) {
            pid_t w = waitpid(-j->pgid, &status, WUNTRACED);
            if (w < 0) {
                if (errno == ECHILD) break;
            } else {
                if (WIFSTOPPED(status)) {
                    j->status = JOB_STOPPED;
                    printf("[%d]+ Stopped\t%s\n", j->jid, j->cmdline);
                    break;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    j->status = JOB_DONE;
                    job_remove_jid(j->jid);
                    break;
                }
            }
        }

        /* restore terminal to shell */
        if (tcsetpgrp(STDIN_FILENO, shell_pgid) < 0) perror("tcsetpgrp restore");
        return 1;
    }

    return 0;
}

/* Execute single command (no pipeline). foreground flag indicates whether to make it foreground job.
 * background_flag is separate: if background_flag=1, we don't wait, and job added as background.
 */
static void execute_single(Command *c, int foreground) {
    if (!c->argv[0]) return;
    if (handle_builtins(c)) return;

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) {
        /* child */

        /* set child process group equal to its pid (new pgid) */
        pid_t childpid = getpid();
        if (setpgid(0, 0) < 0) {
            /* ignore */
        }

        /* restore default signals in child so it reacts to Ctrl-C/Ctrl-Z */
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);

        /* redirections */
        if (c->infile) {
            int fd = open(c->infile, O_RDONLY);
            if (fd < 0) { fprintf(stderr,"failed to open '%s' for input: %s\n", c->infile, strerror(errno)); exit(1); }
            dup2(fd, STDIN_FILENO); close(fd);
        }
        if (c->outfile) {
            int flags = O_WRONLY | O_CREAT | (c->out_append ? O_APPEND : O_TRUNC);
            int fd = open(c->outfile, flags, 0644);
            if (fd < 0) { fprintf(stderr,"failed to open '%s' for output: %s\n", c->outfile, strerror(errno)); exit(1); }
            dup2(fd, STDOUT_FILENO); close(fd);
        }
        if (c->errfile) {
            int fd = open(c->errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { fprintf(stderr,"failed to open '%s' for stderr: %s\n", c->errfile, strerror(errno)); exit(1); }
            dup2(fd, STDERR_FILENO); close(fd);
        }

        execvp(c->argv[0], c->argv);
        fprintf(stderr,"execvp '%s' failed: %s\n", c->argv[0], strerror(errno));
        exit(1);
    } else {
        /* parent */
        /* setpgid for child to ensure pgid set even if child hasn't called setpgid */
        if (setpgid(pid, pid) < 0) {
            /* ignore possible races */
        }

        /* add job to table */
        char cmdline[CMDLINE_LEN];
        strncpy(cmdline, c->argv[0], CMDLINE_LEN-1);
        size_t pos = strlen(cmdline);
        for (int i=1;c->argv[i];++i) {
            strncat(cmdline, " ", CMDLINE_LEN-1 - pos);
            pos = strlen(cmdline);
            strncat(cmdline, c->argv[i], CMDLINE_LEN-1 - pos);
            pos = strlen(cmdline);
        }
        int jid = job_add(pid, cmdline, !foreground);
        Job *j = job_by_jid(jid);

        if (!foreground) {
            printf("[%d] %d\n", j->jid, (int)pid);
            /* background: do not wait */
        } else {
            /* foreground: give terminal control to child pgid */
            if (tcsetpgrp(STDIN_FILENO, pid) < 0) {
                /* warning but continue */
            }

            /* wait for process group to finish or stop */
            int status;
            while (1) {
                pid_t w = waitpid(-pid, &status, WUNTRACED);
                if (w < 0) {
                    if (errno == ECHILD) break;
                    continue;
                }
                if (WIFSTOPPED(status)) {
                    j->status = JOB_STOPPED;
                    j->is_background = 1;
                    printf("\n[%d]+ Stopped\t%s\n", j->jid, j->cmdline);
                    break;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    j->status = JOB_DONE;
                    job_remove_jid(j->jid);
                    break;
                }
            }
            /* restore terminal to shell */
            if (tcsetpgrp(STDIN_FILENO, shell_pgid) < 0) {
                /* ignore */
            }
        }
    }
}

static void execute_pipeline(Command cmds[], int num_cmds, int foreground, int background_flag, char *orig_line) {
    int pipes[MAX_CMDS-1][2];
    pid_t pids[MAX_CMDS];
    pid_t pgid = 0;

    /* create pipes */
    for (int i=0;i<num_cmds-1;++i) {
        if (pipe(pipes[i]) < 0) { perror("pipe"); return; }
    }

    for (int i=0;i<num_cmds;++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); /* cleanup */ for (int k=0;k<i;k++) waitpid(pids[k],NULL,0); return; }
        if (pid == 0) {
            /* CHILD */
            /* set process group: first child becomes leader */
            if (i == 0) {
                if (setpgid(0,0) < 0) { /* ignore */ }
                pgid = getpid();
            } else {
                if (setpgid(0, pgid) < 0) { /* ignore */ }
            }

            /* reset signals to default so children react to Ctrl-C/Ctrl-Z */
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            /* Setup stdin from previous pipe if any */
            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            /* Setup stdout to next pipe if any */
            if (i < num_cmds-1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }

            /* Close all pipe fds in child */
            for (int j=0;j<num_cmds-1;++j) { close(pipes[j][0]); close(pipes[j][1]); }

            /* Apply per-command redirections (child-level) */
            if (cmds[i].infile) {
                int fd = open(cmds[i].infile, O_RDONLY);
                if (fd < 0) { fprintf(stderr,"open infile '%s' failed: %s\n", cmds[i].infile, strerror(errno)); exit(1); }
                dup2(fd, STDIN_FILENO); close(fd);
            }
            if (cmds[i].outfile) {
                int flags = O_WRONLY | O_CREAT | (cmds[i].out_append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].outfile, flags, 0644);
                if (fd < 0) { fprintf(stderr,"open outfile '%s' failed: %s\n", cmds[i].outfile, strerror(errno)); exit(1); }
                dup2(fd, STDOUT_FILENO); close(fd);
            }
            if (cmds[i].errfile) {
                int fd = open(cmds[i].errfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd < 0) { fprintf(stderr,"open errfile '%s' failed: %s\n", cmds[i].errfile, strerror(errno)); exit(1); }
                dup2(fd, STDERR_FILENO); close(fd);
            }

            /* exec */
            if (cmds[i].argv[0] == NULL) exit(0);
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr,"execvp '%s' failed: %s\n", cmds[i].argv[0], strerror(errno));
            exit(1);
        } else {
            /* PARENT */
            /* setpgid for child (race: child may setpgid itself) */
            if (i == 0) {
                pgid = pid;
                if (setpgid(pid, pgid) < 0) { /* ignore */ }
            } else {
                if (setpgid(pid, pgid) < 0) { /* ignore */ }
            }
            pids[i] = pid;

            /* parent closes pipe ends it doesn't need */
            if (i > 0) close(pipes[i-1][0]);
            if (i < num_cmds-1) close(pipes[i][1]);
        }
    }

    /* After forking all children, add job entry */
    int jid = job_add(pgid, orig_line, background_flag);
    Job *j = job_by_jid(jid);

    if (background_flag) {
        printf("[%d] %d\n", j->jid, (int)pgid);
        /* don't wait; leave processes running in background */
    } else {
        /* foreground: give terminal control to pgid */
        if (tcsetpgrp(STDIN_FILENO, pgid) < 0) perror("tcsetpgrp");

        /* wait for the process group */
        int status;
        while (1) {
            pid_t w = waitpid(-pgid, &status, WUNTRACED);
            if (w < 0) {
                if (errno == ECHILD) break;
            } else {
                if (WIFSTOPPED(status)) {
                    job_mark_stopped(pgid);
                    printf("\n[%d]+ Stopped\t%s\n", j->jid, j->cmdline);
                    break;
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    job_mark_done(pgid);
                    job_remove_jid(j->jid);
                    break;
                }
            }
        }
        /* restore terminal to shell */
        if (tcsetpgrp(STDIN_FILENO, shell_pgid) < 0) perror("tcsetpgrp restore");
    }
}

/* Main */
int main(void) {
    char raw[MAX_INPUT];
    char spaced[MAX_INPUT*2];
    char *tokens[MAX_TOKENS];

    /* Initialize job table */
    memset(jobs, 0, sizeof(jobs));

    /* Ensure shell is running in its own process group */
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0) {
        /* Might fail if already in pg, ignore */
    }
    /* Take control of terminal */
    if (isatty(STDIN_FILENO)) {
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        tcgetattr(STDIN_FILENO, &shell_tmodes);
    }

    /* Install signal handlers in shell: ignore SIGINT and SIGTSTP so shell doesn't terminate or stop */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) perror("sigaction SIGCHLD");

    /* ignore SIGINT and SIGTSTP in shell */
    signal(SIGINT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    /* ignore SIGQUIT to be safe */
    signal(SIGQUIT, SIG_IGN);
    /* ignore SIGTTOU, SIGTTIN to prevent background stops when changing terminal control */
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);

    while (1) {
        /* check and reap children from SIGCHLD */
        check_sigchld_and_reap();

        printf("tsh> ");
        fflush(stdout);

        if (!fgets(raw, sizeof(raw), stdin)) {
            printf("\nExiting TinyShell...\n");
            break;
        }
        raw[strcspn(raw,"\n")] = '\0';
        char *line = trim(raw);
        if (!line || line[0] == '\0') continue;

        /* quick exit */
        if (strcmp(line, "exit") == 0) break;

        /* prepare spaced string so operators are tokens */
        space_operators(line, spaced, sizeof(spaced));

        /* tokenize */
        int ntokens = tokenize(spaced, tokens, MAX_TOKENS);
        if (ntokens == 0) continue;

        /* detect background operator & : if last token is "&", mark background and remove it */
        int background_flag = 0;
        if (ntokens > 0 && strcmp(tokens[ntokens-1], "&") == 0) {
            background_flag = 1;
            tokens[ntokens-1] = NULL;
            ntokens--;
            if (ntokens == 0) continue;
        }

        /* parse into commands */
        Command cmds[MAX_CMDS];
        int ncmds = parse_pipeline(tokens, ntokens, cmds, MAX_CMDS);
        if (ncmds < 0) { fprintf(stderr,"parse error\n"); continue; }
        if (ncmds == 0) continue;

        /* If single command, handle builtins (including fg/bg/jobs) */
        if (ncmds == 1) {
            /* check fg/bg builtins also via handle_builtins */
            if (handle_builtins(&cmds[0])) {
                /* builtin handled */
            } else {
                execute_single(&cmds[0], !background_flag);
            }
        } else {
            /* pipeline: execute with background/foreground flag */
            execute_pipeline(cmds, ncmds, !background_flag, background_flag, line);
        }
    }

    return 0;
}