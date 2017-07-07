#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <assert.h>

//******************************************************************************
// Macros
#define MAXARGS       20   //maximum number of arugments
#define MAX_CWD       1024 //limit on cwd string length
#define MAX_PROCESSES 20   //limit on number of background processes
#define KILL_ON_EXIT  0    //kill background processes on exit - bash doesn't (change to 1 to kill)
//******************************************************************************

//******************************************************************************
//  Global vars
pid_t pid_fg; //foreground child pid
pid_t pid_bg[MAX_PROCESSES]; // background pid array
char *cmd_bg[MAX_PROCESSES]; // background command args, formatted
FILE *redir_stdout_fg;       // foreground stdout redirected
int   pipefd_fg[2];          // piping
//******************************************************************************

//****************************************************************************
//   Foreground jobs related functions
//****************************************************************************
void fg_wait(pid_t pid) { //foreground wait
    pid_fg = pid;  // for Ctrl-C handler
    int status = 0;
    pid_t wait = waitpid(pid, &status, 0);
    pid_fg = 0;
    if ( redir_stdout_fg ) {
      fclose(redir_stdout_fg);
        redir_stdout_fg = NULL;
    }
}

//****************************************************************************
//   Background jobs related functions
//****************************************************************************
int get_bgID(void) {
    int id = -1;
    for(int i = 0; i < MAX_PROCESSES; i++) {
        if(!pid_bg[i]) {
            id = i;
            break;
        }
    }
    return id;
}
void clr_bgEntry(int id) {
    assert(id >= 0 && id < MAX_PROCESSES);
    free(cmd_bg[id]);
    cmd_bg[id] = NULL;
    pid_bg[id] = 0;
}
void free_bgEntries(void) {
    for(int i = 0; i < MAX_PROCESSES; i++) {
#if KILL_ON_EXIT
        kill(pid_bg[i], SIGKILL);
#endif
        clr_bgEntry(i);
    }
}
void set_bgEntry(pid_t pid, int id, char *args[]) {
    assert(id >= 0 && id < MAX_PROCESSES);
    assert(pid_bg[id]==0);
    //compute length of formatted cmd line
    size_t len = 0;
    for(int i = 1; i < MAXARGS; i++) {
        if(!args[i]) break; //end of args?
        len += strlen(args[i])+1;
    }
    //allocate memory for cmd_bg[i] and save formatted args as a single string
    if((cmd_bg[id] = (char*) malloc(len+1)) != NULL) {
        size_t j = 0;
        for(int i = 1; i < MAXARGS; i++) {
            if(!args[i]) break; //end of args?
            char *p = args[i];
            while(*p) cmd_bg[id][j++] = *p++;
            cmd_bg[id][j++] = ' ';
        }
        cmd_bg[id][j] = 0; //terminates for formatted string
        assert(j == len);
    }
    else printf("System out of memory, malloc failed\n");
    pid_bg[id] = pid;
}

void check_bgFinished(void) {
    for(int i = 0; i < MAX_PROCESSES; i++) {
        if ( pid_bg[i] ) {
            int status = -1;
            pid_t result = waitpid(pid_bg[i], &status, WNOHANG);
            if (result == 0) {         // Child still alive
            } else if (result == -1) { // Error 
            } else {                   // Child exited
                printf("job [%d] exited\n",i);
                clr_bgEntry(i);
            }
        }
    }
}

//****************************************************************************
//   Args related functions
//****************************************************************************
void printArgs(const int cnt, char *args[]) { //for debugging
    printf("%s%d\n", "cnt = ", cnt);
    for(int i = 0; i < MAXARGS; i++) {
        if(args[i] != NULL) printf("%s\n", args[i]);
    }
}

void initArgs(char *args[]) {
    for(int i = 0; i < MAXARGS; i++) {
        args[i] = NULL;
    }
}

void freeArgs(char *args[]) {
    free(args[0]);
    initArgs(args);
}

int get_argCnt(char *args[]) { //get argument count
    int n = 1;
    for(; n < MAXARGS && args[n] != NULL; n++) ;
    return n;
}

int isRedirected(char *args[]) { 
    int res = 0;
    int ac = get_argCnt(args); 
    if(ac > 2) {
        int loc = ac-2; //location of second to last argument
        if(strlen(args[loc]) == 1 && args[loc][0] == '>') res = 1;
    }
    return res;
}

char* getRedir(char *args[]) { // get redirerction file name
    assert(isRedirected(args));
    int ac = get_argCnt(args); 
    char *res = args[ac-1];
    return res;
}

void maskRedir(char *args[]) { // redirection args are cleared
    int ac = get_argCnt(args); 
    if(ac > 2) {
        int loc = ac-2; //location of second to last argument
        if(strlen(args[loc]) == 1 && args[loc][0] == '>') {
            args[loc] = args[loc+1] = NULL;
        }
    }
}

int isPiped(char *args[]) { 
    int res = 0;
    int ac = get_argCnt(args); 
    for ( int i = 1; i < ac; i++ ) {
        if(strlen(args[i]) == 1 && args[i][0] == '|') res++;
    }
    return res;
}

void maskAfterPipe(char *args[]) { // after pipe args are cleared
    int ac  = get_argCnt(args); 
    int loc = 0;
    for ( int i = 1; i < ac; i++ ) { 
        if(strlen(args[i]) == 1 && args[i][0] == '|') loc = i;
        if ( loc ) args[i] = NULL;
    }
}

int getAfterPipe(char *args[]) { // get command after pipe
    int ac  = get_argCnt(args); 
    int loc = 0;
    for ( int i = 1; i < ac; i++ ) { 
        if(strlen(args[i]) == 1 && args[i][0] == '|') loc = i;
        if ( loc ) break;
    }
    return loc + 1;
}

//****************************************************************************
//   getcmd
//****************************************************************************
int getcmd(const char *prompt, char *args[], int *background) {
    ssize_t length = 0;
    int res = 0;
    char *token = NULL; //pointers to tokens
    char *line  = NULL; //pointer to line
    size_t linecap = 0;
    int empty = 1;

    //read non empty line
    while(!length) {
        fflush(stdout); printf("%s", prompt); fflush(stdout);
        length = getline(&line, &linecap, stdin);
        check_bgFinished(); // bash checks finished jobs afer each \n
        if(length < 0) return -1;

        //check for white space; detect background
        empty = 1; //reinitialize upon loop
        for(ssize_t i = length; i > 0; i--) { //replace spaces to common space for ease of separation later
            if(isspace(line[i - 1])) {
                line[i - 1] = ' ';
            }
            else {
                if(line[i - 1] == '&' && empty) { //set
                    *background = 1;
                    line[i - 1] = ' ';
                }
                else empty = 0;
            }
        }
        if(!empty) break;
        GETAGAIN:
        *background = 0;
        length = 0;
        free(line); line = NULL;
    }
    args[0] = line; //Remember where allocated memory starts
    
    while((token = strsep(&line, " \t\n")) != NULL) {
        if(res+2 > MAXARGS) {
            printf("%s", "Unrecognized command, too many arguments");
            res = 0;
            goto GETAGAIN;
        }
        if(strlen(token)) args[++res] = token;
    }

    return res;
}

//****************************************************************************
//   Signal Handlers
//****************************************************************************
void INThandler(int sig) {
    signal(SIGINT, SIG_IGN); // ignore another Ctrl-C while this one processed
    if(pid_fg) kill(pid_fg, SIGKILL); // this kill cannot be ignored
    pid_fg = 0;
    signal(SIGINT, INThandler); // reenable Ctrl-C handler
}

void STPhandler(int sig) {
    printf("\b\b  \b\b"); // erase ^Z from terminal
    fflush(stdout);
}

void startSigHandlers(void) {
    if ( signal(SIGINT,  INThandler) == SIG_ERR ) { //set Ctrl-C handler
        printf("ERROR! Could not bind Ctrl-C handler\n");
        exit(1);
    }
    if ( signal(SIGTSTP, STPhandler) == SIG_ERR ) {; //set Ctrl-Z handler
        printf("ERROR! Could not bind Ctrl-Z handler\n");
        exit(1);
    }
}

//****************************************************************************
//   Built-in functions
//****************************************************************************
void bi_cd(char *dir)  {
    if(dir != NULL) {
        if(chdir(dir)) printf("cd failed\n");
    } else { //bash changes to home directory if no directory is given
        const char *homedir;
        if((homedir = getenv("HOME")) == NULL) {
            homedir = getpwuid(getuid())->pw_dir;
        }
        if(chdir(homedir)) printf("cd failed\n");
    }
}
void bi_pwd(void) {
    char cwd[MAX_CWD];
    if(getcwd(cwd, sizeof(cwd)) != NULL) printf("%s\n", cwd);
    else printf("pwd error\n");
}
void bi_exit(char *line) {
    free(line);
    free_bgEntries();
    exit(0);
}
void bi_fg(char *idstr) {
    //convert idstr to int
    int  ok = idstr != NULL;
    char *p = idstr; //Check if all decimal digits before atoi
    if (ok) while(*p) if(!isdigit(*p++)) {ok = 0; break;}
    if(ok) {
        int id = atoi(idstr);
        //printf("A\n%d",id);
        if(id > MAX_PROCESSES || !pid_bg[id]) {printf("No such job\n"); fflush(stdout);}
        else {
            pid_t pid = pid_bg[id];
            clr_bgEntry(id);
            fg_wait(pid);
        }
    } else printf("Improper job number\n");
}
void bi_jobs(void) {
    for(int i = 0; i < MAX_PROCESSES; i++) {
        if(pid_bg[i]) printf("[%d]\t%s\n", i, cmd_bg[i]);
    }
}
int builtIn(char *args[]) {
    int result = 1;
    if(strcmp(args[1], "cd"  ) == 0) bi_cd(args[2]);
    else if(strcmp(args[1], "pwd" ) == 0) bi_pwd();
    else if(strcmp(args[1], "exit") == 0) bi_exit(args[0]);
    else if(strcmp(args[1], "fg"  ) == 0) bi_fg(args[2]);
    else if(strcmp(args[1], "jobs") == 0) bi_jobs();
    else result = 0;

    return result; //if not built in
}

//****************************************************************************
//   redirection related
//****************************************************************************

FILE *re_open(char* name)
{
    FILE *f = fopen(name,"w");
    if ( f == NULL ) printf("Error! Unable to open file %s\n",name);
    return f;
}

//****************************************************************************
//   pipe related
//****************************************************************************

int pi_open( int n ) {
    if ( n == 1 ) {
        if( pipe(pipefd_fg) != 0 ) {
            printf("Error! faile to create pipe\n");
        } else return 1;
    } else if ( n > 1 ) {
        printf("too many pipes\n");
    }
    return 0;
}

//****************************************************************************
//   main
//****************************************************************************
int main(void) {
    char *args[MAXARGS];
    int bg = 0;
    initArgs(args);
    
    startSigHandlers();

    while(1) {
        bg = 0;

        int cnt = getcmd("\n >> ", args, &bg);
        //printArgs(cnt, args);
        if(!builtIn(args)) {
            pid_t pid   = 0;
            int   bgid  = bg ? get_bgID() : -1;
            int   piped = isPiped(args);
            if(bg && bgid < 0) {
                printf("Command failed, too many background processes\n");
            }
            else {
                if (!piped && !bg && isRedirected(args)) {
                    redir_stdout_fg = re_open(getRedir(args));
                }
                if (piped && !pi_open(piped)) continue;
                
                pid = fork();
                if(pid == -1) {
                    printf("Forking failed\n");
                }
                else if(pid == 0) { //child 1
                     if (redir_stdout_fg != NULL) {
                       //printf("fileno %d, %d\n",fileno(redir_stdout_fg),STDOUT_FILENO);
                       dup2(fileno(redir_stdout_fg), STDOUT_FILENO);
                       fclose(redir_stdout_fg);
                     }
                     maskRedir(args);
                     if (piped) {
                        dup2(pipefd_fg[STDOUT_FILENO], STDOUT_FILENO); 
                        close(pipefd_fg[STDIN_FILENO]); // not used by pre-pipe
                        maskAfterPipe(args);
                     }
                     signal(SIGINT, SIG_IGN); // Ctrl-C must not kill bg processes
                     signal(SIGTSTP,SIG_IGN); // Ctrl-Z must be ignored
                     execvp(args[1], args+1);
                     printf("Error! Command failed\n");
                     _exit(1); // exit child in case execvp did not work
                }
                else { 
                    if (!piped && bg == 1) {
                        set_bgEntry(pid,bgid,args);
                    } 
                    else {
                        if (piped) {
                            pid_t pid2 = fork(); // for after pipe command child 2
                            if ( pid2 == -1 ) { printf("Forking 2 failed\n"); }
                            else if (pid2 == 0 ) {
                                dup2(pipefd_fg[STDIN_FILENO], STDIN_FILENO); 
                                close(pipefd_fg[STDOUT_FILENO]); // not used by after-pipe
                                maskRedir(args); // ignore redirection if any
                                int loc = getAfterPipe(args);
                                signal(SIGTSTP,SIG_IGN); // Ctrl-Z must be ignored
                                execvp(args[loc], args+loc);
                                printf("Error! Command failed\n");
                                _exit(1); // exit child in case execvp did not work
                            } else {
                                close(pipefd_fg[STDIN_FILENO]);  // not used by parent
                                close(pipefd_fg[STDOUT_FILENO]);
                                pipefd_fg[STDIN_FILENO] = pipefd_fg[STDOUT_FILENO] = 0;
                                fg_wait(pid2);
                            }
                        }
                        fg_wait(pid);
                    }
                }
            }
        }
        freeArgs(args); //free memory
        }
    return 0;
}
