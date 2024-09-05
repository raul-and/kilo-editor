/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8
#define KILO_QUIT_TIMES 1

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
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsgTime;
    struct termios origTermios;
};

struct editorConfig conf;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void(*callback)(char *, int));

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

int editorRowRxToCx(editorRow *eRow, int rx) {
  int cur_rX = 0;
  int cX;
  for (cX = 0; cX < eRow->tSize; cX++) {
    if (eRow->text[cX] == '\t')
      cur_rX += (KILO_TAB_STOP - 1) - (cur_rX % KILO_TAB_STOP);
    cur_rX++;
    if (cur_rX > rx) return cX;
  }
  return cX;
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

void editorInsertRow(int at, char *s, size_t len){
    if (at < 0 || at > conf.numRows) return;

    conf.eRow = realloc(conf.eRow, sizeof(editorRow) * (conf.numRows+1));
    memmove(&conf.eRow[at+1], &conf.eRow[at], sizeof(editorRow) * (conf.numRows - at));

    conf.eRow[at].tSize = len;
    conf.eRow[at].text = malloc(len + 1);
    memcpy(conf.eRow[at].text, s, len);
    conf.eRow[at].text[len] = '\0';
    
    conf.eRow[at].rSize = 0;
    conf.eRow[at].render = NULL;
    editorUpdateRow(&conf.eRow[at]);
    
    conf.numRows++;
    conf.dirty++;
}

void editorFreeRow(editorRow *eRow){
    free(eRow->text);
    free(eRow->render);
}

void editorDelRow(int at){
    if (at < 0 || at >= conf.numRows) return;
    editorFreeRow(&conf.eRow[at]);
    memmove(&conf.eRow[at], &conf.eRow[at+1], sizeof(editorRow)*(conf.numRows - at - 1));
    conf.numRows--;
    conf.dirty++;
}

void editorRowInsertChar(editorRow *erow, int at, int c){
    if (at < 0 || at > erow->tSize) at = erow->tSize;
    erow->text = realloc(erow->text, erow->tSize + 2);
    memmove(&erow->text[at+1], &erow->text[at], erow->tSize - at + 1);
    erow->tSize++;
    erow->text[at] = c;
    editorUpdateRow(erow);
}

void editorRowAppendString(editorRow *eRow, char *s, size_t len){
    eRow->text = realloc(eRow->text, eRow->tSize + len + 1);
    memcpy(&eRow->text[eRow->tSize], s, len);
    eRow->tSize += len;
    eRow->text[eRow->tSize] = '\0';
    editorUpdateRow(eRow);
    conf.dirty++;
}

void editorRowDelChar(editorRow *eRow, int at){
    if (at < 0 || at >= eRow->rSize) return;
    memmove(&eRow->text[at], &eRow->text[at+1], eRow->tSize - at);
    eRow->tSize--;
    editorUpdateRow(eRow);
    conf.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c){
    if (conf.cY == conf.numRows)
        editorInsertRow(conf.numRows, "", 0);

    editorRowInsertChar(&conf.eRow[conf.cY], conf.cX, c);
    conf.cX++;
}

void editorInsertNewLine(){
    if (conf.cX == 0)
        editorInsertRow(conf.cY, "", 0);
    else {
        editorRow *eRow = &conf.eRow[conf.cY];
        editorInsertRow(conf.cY+1, &eRow->text[conf.cX], eRow->tSize - conf.cX);
        eRow = &conf.eRow[conf.cY];
        eRow->tSize = conf.cX;
        eRow->text[eRow->tSize] = '\0';
        editorUpdateRow(eRow);
    }
    conf.cY++;
    conf.cX = 0;
}

void editorDelChar(){
    if (conf.cY == conf.numRows) return;
    if (conf.cX == 0 && conf.cY == 0) return;


    editorRow *eRow = &conf.eRow[conf.cY];
    if (conf.cX > 0){
        editorRowDelChar(eRow, conf.cX - 1);
        conf.cX--;
    } else {
        conf.cX = conf.eRow[conf.cY - 1].tSize;
        editorRowAppendString(&conf.eRow[conf.cY-1], eRow->text, eRow->tSize);
        editorDelRow(conf.cY);
        conf.cY--;
    }
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

        editorInsertRow(conf.numRows, line, lineLen);
    }

    free(line);
    fclose(fp); 
    conf.dirty = 0;   
}

void editorSave(){
    if (conf.filename == NULL){
        conf.filename = editorPrompt("Save as: %s", NULL);
        if (conf.filename == NULL){
            editorSetStatusMessage("Save canceled");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(conf.filename, O_RDWR | O_CREAT, 0664);
    if (fd != -1){
        if (ftruncate(fd, len) != -1){
            if (write(fd, buf, len) == len){
                close(fd);
                free(buf);
                conf.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }    

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

void editorFindCallback(char *query, int key){
    static int last_match = -1;
    static int direction = 1;
    
    if (key == '\r' || key == '\x1b'){
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int curr = last_match;
    for (int i = 0; i < conf.numRows; i++){
        curr += direction;
        if (curr == -1) curr = conf.numRows-1;
        else if (curr == conf.numRows) curr = 0;

        editorRow *eRow = &conf.eRow[curr];
        char *match = strstr(eRow->render, query);
        if (match){
            last_match = curr;
            conf.cY = curr;
            conf.cX = editorRowRxToCx(eRow, match - eRow->render);
            conf.rowOff = conf.numRows;
            break;
        }
    }
}

void editorFind(){
    int saved_cX = conf.cX;
    int saved_cY = conf.cY;
    int saved_colOff = conf.colOff;
    int save_rowOff = conf.rowOff;
    
    char *query = editorPrompt("Search: %s (Use ARROWS/ENTER/ESC)", editorFindCallback);

    if (query)
        free(query);
    else {
        conf.cX = saved_cX;
        conf.cY = saved_cY;
        conf.colOff = saved_colOff;
        conf.rowOff = save_rowOff;
    }
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
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                        conf.filename ? conf.filename: "[No name]", conf.numRows,
                        conf.dirty ? "(modified)" : "");
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

char *editorPrompt(char *prompt, void(*callback)(char *, int)){
    size_t bufSize = 128;
    char *buf = malloc(bufSize);

    size_t bufLen = 0;
    buf[0] = '\0';

    while (1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int key = editorReadKey();
        if (key == DEL_KEY || key == CTRL_KEY('h') || key == BACKSPACE){
            if (bufLen != 0) buf[--bufLen] = '\0';
        } else if (key == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, key);
            free(buf);
            return NULL;
        } else if (key == '\r') {
            if (bufLen != 0) {
                editorSetStatusMessage("");
                if (callback) callback(buf, key);
                return buf;
        }
        } else if (!iscntrl(key) && key < 128) {
            if (bufLen == bufSize - 1) {
                bufSize *= 2;
                buf = realloc(buf, bufSize);
        }

        buf[bufLen++] = key;
        buf[bufLen] = '\0';
        }
        
        if (callback) callback(buf, key);
    }
}

void editorMoveCursor(int key){
    editorRow *row = (conf.cY >= conf.numRows) ? NULL : &conf.eRow[conf.cY];
    
    switch(key){
        case ARROW_UP:
            if(conf.cY != 0){
                conf.cY--;
                conf.cX = conf.eRow[conf.cY].rSize;
            }
            break;
            
        case ARROW_LEFT:
            if(conf.cX != 0)
                conf.cX--;
            else if (conf.cY > 0)
                conf.cY--;
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
    static int quitTimes = KILO_QUIT_TIMES;
    
    int key = editorReadKey();

    switch (key){
        case '\r':
            editorInsertNewLine();
            break;

        case CTRL_KEY('q'):
            if(conf.dirty && quitTimes > 0){
                editorSetStatusMessage("WARNING!!! File has unsave changes. "
                "Press Ctrl-Q %d more time(s) to quit", quitTimes);
                quitTimes--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        case CTRL_KEY('s'):
            editorSave();
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
            if (key == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
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

    quitTimes = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor(){
    conf.cX = 0;
    conf.cY = 0;
    conf.rX = 0;
    conf.rowOff = 0;
    conf.colOff = 0;
    conf.eRow = NULL;
    conf.dirty = 0;
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
    
    editorSetStatusMessage("HELP: Ctrl-Q = save | Ctrl-F = find | Ctrl-Q = quit");

    while (1){
        editorRefreshScreen();
        editorProcessKeypress();
    };

    return 0; 
}
