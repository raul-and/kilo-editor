/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(key) ((key) & 0x1f)

enum editorKey{
    ARROW_UP = 1000,
    ARROW_DOWN,
    ARROW_LEFT,
    ARROW_RIGHT,
    DEL_KEY,
    PAGE_UP,        //REPAG
    PAGE_DOWN,      //AVPAG
    HOME_KEY,
    END_KEY
};

/*** data ***/

typedef struct editorRow{
    int size;
    char *text;
} editorRow;

struct editorConfig {
    int cx, cy;
    int rowOff;
    int screenRows;
    int screenCols;
    int numrows;
    editorRow *erow;
    struct termios origTermios;
};

struct editorConfig conf;

/*** terminal ***/

void die(const char *s){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode(){
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &conf.origTermios) != 1)
        die("tcsetattr");
}

void enableRawMode(){
    if (tcgetattr(STDIN_FILENO, &conf.origTermios) == -1) die("tcsetattr");
    atexit(disableRawMode);

    struct termios raw = conf.origTermios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(){
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9'){
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                
                if (seq[2] == '~')
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
            }
            else
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
        } else if (seq[0] == 'O'){
            switch (seq[1]){
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else 
        return c;
}

int getCursorPosition(int *rows, int *cols){
    char buf[32];
    unsigned int i=0;

    if (write(STDOUT_FILENO, "\x1b[6b", 4) != 4) return -1;
    
    while (i < sizeof(buf)-1){
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return getCursorPosition(rows, cols);
    }
    else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

void editorAppendRow(char *s, size_t len){
    conf.erow = realloc(conf.erow, sizeof(editorRow) * ((conf.numrows) + 1));

    int at = conf.numrows;
    conf.erow[at].size = len;
    conf.erow[at].text = malloc(len + 1);
    memcpy(conf.erow[at].text, s, len);
    conf.erow[at].text[len] = '\0';
    conf.numrows++;
}

/*** file i/o ***/

void editorOpen(char *fileName){
    FILE *fp = fopen(fileName, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t lineCap = 0;
    ssize_t lineLen;

    while ((lineLen = getline(&line, &lineCap, fp)) != -1){
        while (lineLen > 0 && (line[lineLen - 1] == '\n' || line[lineLen - 1] == '\r')) 
            lineLen--;

        editorAppendRow(line, lineLen);
    }

    free(line);
    fclose(fp);    
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len){
    char *new= realloc(ab->b, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}

/*** output ***/

void editorDrawRows(struct abuf *ab){
    int y;
    for (y = 0; y < conf.screenRows; y++){
        if (y >= conf.numrows){
            if(conf.numrows == 0 && y == conf.screenRows / 3){
                char welcome[80];
                int welcomeLen = snprintf(welcome, sizeof(welcome),
                    "Kilo editor -- version %s", KILO_VERSION);
                if (welcomeLen > conf.screenCols) welcomeLen = conf.screenCols;

                int padding = ((conf.screenCols - welcomeLen) / 2);
                if (padding){
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomeLen);
            }
            else
                abAppend(ab, "~", 1);

        } else {
            int len = conf.erow[y].size;
            if (len > conf.screenCols) len = conf.screenCols;
            abAppend(ab, conf.erow[y].text, len);
        }

        abAppend(ab, "\x1b[K", 3);

        if (y < conf.screenRows - 1)
            abAppend(ab, "\r\n", 2); 
    }
}

void editorRefreshScreen(){
    struct abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", conf.cy + 1, conf.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key){
    switch(key){
        case ARROW_UP:
        if(conf.cy != 0)
            conf.cy--;
        break;
        case ARROW_LEFT:
        if(conf.cx != 0)
            conf.cx--;
        break;
        case ARROW_DOWN:
        if(conf.cy != conf.screenRows-1)
            conf.cy++;
        break;
        case ARROW_RIGHT:
        if(conf.cx != conf.screenCols-1)
            conf.cx++;
        break;
    }
}

void editorProcessKeypress(){
    int key = editorReadKey();

    switch (key){
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(key);
            break;

        case HOME_KEY:
            conf.cx = 0;
            break;

        case END_KEY:
            conf.cx = conf.screenCols-1;
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                int times = conf.screenRows;
                while (times--)
                    editorMoveCursor(key == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
    }
}

/*** init ***/

void initEditor(){
    conf.cx = 0;
    conf.cy = 0;
    conf.erow = NULL;
    
    if (getWindowSize(&conf.screenRows, &conf.screenCols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);
    
    while (1){
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0; 
}