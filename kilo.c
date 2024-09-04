/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/types.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

#define CTRL_KEY(key) ((key) & 0x1f)

enum editorKey{
    BACKSPACE = 127,
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
    char *text;
    int tSize;
    char *render;
    int rSize; 
} editorRow;

struct editorConfig {
    int cX, cY;
    int rX;
    int rowOff;
    int colOff;
    int screenRows;
    int screenCols;
    int numRows;
    editorRow *eRow;
    char *filename;
    char statusmsg[80];
    time_t statusmsgTime;
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

int editorRowCxToRx(editorRow *erow, int cX){
    int rX =0;
    for (int j = 0; j < cX; j++){
        if(erow->text[j] == '\t')
            rX += (KILO_TAB_STOP - 1) - (rX % KILO_TAB_STOP);
        rX++;
    }
    
    return rX;
}

void editorUpdateRow(editorRow *erow){
    int tabs = 0;
    for (int j = 0; j < erow->tSize; j++)
        if (erow->text[j] == '\0') tabs++;
    
    free(erow->render);
    erow->render = malloc(erow->tSize + tabs*(KILO_TAB_STOP-1) + 1);

    int idx = 0;
    for (int j = 0; j < erow->tSize; j++){
        if(erow->text[j] == '\t'){
            erow->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0) erow->render[idx++] = ' ';
        } else
            erow->render[idx++] = erow->text[j];
    }

    erow->render[idx] = '\0';
    erow->rSize = idx;
}

void editorAppendRow(char *s, size_t len){
    conf.eRow = realloc(conf.eRow, sizeof(editorRow) * ((conf.numRows) + 1));

    int at = conf.numRows;
    conf.eRow[at].tSize = len;
    conf.eRow[at].text = malloc(len + 1);
    memcpy(conf.eRow[at].text, s, len);
    conf.eRow[at].text[len] = '\0';
    
    conf.eRow[at].rSize = 0;
    conf.eRow[at].render = NULL;
    editorUpdateRow(&conf.eRow[at]);
    
    conf.numRows++;
}

void editorRowInsertChar(editorRow *erow, int at, int c){
    if (at < 0 || at > erow->tSize) at = erow->tSize;
    erow->text = realloc(erow->text, erow->tSize + 2);
    memmove(&erow->text[at+1], &erow->text[at], erow->tSize - at + 1);
    erow->tSize++;
    erow->text[at] = c;
    editorUpdateRow(erow);
}

/*** editor operations ***/

void editorInsertChar(int c){
    if (conf.cY == conf.numRows)
        editorAppendRow("", 0);
    editorRowInsertChar(&conf.eRow[conf.cY], conf.cX, c);
    conf.cX++;
}

/*** file i/o ***/

char *editorRowsToString(int *buflen){
    int totLen = 0;
    for (int j = 0; j < conf.numRows; j++)
        totLen += conf.eRow[j].tSize + 1;
    *buflen = totLen;

    char *buf = malloc(totLen);
    char *p = buf;

    for(int j = 0; j < conf.numRows; j++){
        memcpy(p, conf.eRow[j].text, conf.eRow[j].tSize);
        p += conf.eRow[j].tSize;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *fileName){
    free(conf.filename);
    conf.filename = strdup(fileName);
    
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

void editorScroll(){
    conf.rX = 0;
    if (conf.cY < conf.numRows)
        conf.rX = editorRowCxToRx(&conf.eRow[conf.cY], conf.cX);

    if (conf.cY < conf.rowOff)
        conf.rowOff = conf.cY;

    if (conf.cY >= conf.rowOff + conf.screenRows)
        conf.rowOff = conf.cY - conf.screenRows + 1;

    if (conf.rX < conf.colOff)
        conf.colOff = conf.rX;
    
    if (conf.rX >= conf.colOff + conf.screenCols)
        conf.colOff = conf.rX - conf.screenCols + 1;
}

void editorDrawRows(struct abuf *ab){
    int y;
    for (y = 0; y < conf.screenRows; y++){
        int fileRow = y + conf.rowOff;
        if (fileRow >= conf.numRows){
            if(conf.numRows == 0 && y == conf.screenRows / 3){
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
            int len = conf.eRow[fileRow].rSize - conf.colOff;
            if (len < 0) len = 0;
            if (len > conf.screenCols) len = conf.screenCols;
            abAppend(ab, &conf.eRow[fileRow].render[conf.colOff], len);
        }

        abAppend(ab, "\x1b[K", 3);

        abAppend(ab, "\r\n", 2); 
    }
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                        conf.filename ? conf.filename: "[No name]", conf.numRows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", conf.cY + 1, conf.numRows);
    if (len > conf.screenCols) len = conf.screenCols;
    abAppend(ab, status, len);

    while (len < conf.screenCols){
        if (conf.screenCols - len == rlen){
            abAppend(ab, rstatus, rlen);
            break;
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab){\
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(conf.statusmsg);
  if (msglen > conf.screenCols) msglen = conf.screenCols;
  if (msglen && time(NULL) - conf.statusmsgTime < 5)
    abAppend(ab, conf.statusmsg, msglen);
}

void editorRefreshScreen(){
    editorScroll();
    
    struct abuf ab = ABUF_INIT;
    
    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (conf.cY - conf.rowOff) + 1,
                                              (conf.rX - conf.colOff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(conf.statusmsg, sizeof(conf.statusmsg), fmt, ap);
    va_end(ap);
    conf.statusmsgTime = time(NULL);
}

/*** input ***/

void editorMoveCursor(int key){
    editorRow *row = (conf.cY >= conf.numRows) ? NULL : &conf.eRow[conf.cY];
    
    switch(key){
        case ARROW_UP:
            if(conf.cY != 0)
                conf.cY--;
            break;
            
        case ARROW_LEFT:
            if(conf.cX != 0)
                conf.cX--;
            else if (conf.cY > 0){
                conf.cY--;
                conf.cX = conf.eRow[conf.cY].tSize;
            }
            break;

        case ARROW_DOWN:
            if(conf.cY < conf.numRows)
                conf.cY++;
            break;

        case ARROW_RIGHT:
            if (row && conf.cX < row->tSize)
                conf.cX++;
            else if (row && conf.cX == row->tSize){
                conf.cY++;
                conf.cX = 0;
            }
            break;

        row = (conf.cY >= conf.numRows ? NULL : &conf.eRow[conf.cY]);
        int rowLen = row ? row->tSize : 0;
        if (conf.cX > rowLen)
            conf.cX = rowLen;
    }
}

void editorProcessKeypress(){
    int key = editorReadKey();

    switch (key){
        case 'r':
            /*todo*/
            break;

        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case HOME_KEY:
            conf.cX = 0;
            break;

        case END_KEY:
            if (conf.cY < conf.numRows)
                conf.cX = conf.eRow[conf.cY].tSize;
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            /*todo*/
            break;

        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (key == PAGE_UP)
                    conf.cY = conf.rowOff;
                else if (key == PAGE_DOWN){
                    conf.cY = conf.rowOff + conf.screenRows -1;
                    if (conf.cY > conf.numRows) conf.cY = conf.numRows;
                }

                int times = conf.screenRows;
                while (times--)
                    editorMoveCursor(key == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(key);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(key);
            break;
    }
}

/*** init ***/

void initEditor(){
    conf.cX = 0;
    conf.cY = 0;
    conf.rX = 0;
    conf.rowOff = 0;
    conf.colOff = 0;
    conf.eRow = NULL;
    conf.filename = NULL;
    conf.statusmsg[0] = '\0';
    conf.statusmsgTime = 0;
    
    if (getWindowSize(&conf.screenRows, &conf.screenCols) == -1) die("getWindowSize");
    conf.screenRows -= 2;
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2)
        editorOpen(argv[1]);
    
    editorSetStatusMessage("HELP: Ctrl-Q: quit");

    while (1){
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0; 
}