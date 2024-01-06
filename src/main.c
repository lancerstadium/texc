/**
 * @file main.c
 * @author lancer (lancerstadium@163.com)
 * @brief 主程序入口
 * @version 0.1
 * @date 2024-01-05
 * @copyright Copyright (c) 2024
 * @note 参考项目：[文本编辑器 | kilo](https://viewsourcecode.org/snaptoken/kilo)
 */


// ======================================================================= //
//                               Includes
// ======================================================================= //

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <stdlib.h>

// ======================================================================= //
//                                Defines
// ======================================================================= //
/** `texc`版本 */
#define TEXC_VERSION "0.0.1"

/** 切换宏`CRTL+k`：按位对二进制值`00011111`进行`AND`。 */
#define CTRL_KEY(k) ((k) & 0x1f)
/** 制表位的长度常量 */
#define TAB_STOP 8
/** 如果设置了`ec.dirty`，将在状态栏中显示警告：要求用户再按`Ctrl-Q`两次才能退出而不保存 */
#define QUIT_TIMES 2

#define HL_SYN_NUMBERS   (1 << 0)
#define HL_SYN_STRINGS   (1 << 1)

/**
 * @brief 编辑器控制键入配置
 * @note 按键冲突处理
 * 现在我们只需在`editor_key`枚举中选择
 * 与`wasd`不冲突的箭头键表示形式即可。
 * 我们将为它们提供一个超出字符范围的大整数值，
 * 以便它们不会与任何普通按键冲突。
 */
enum editor_key {
    BACK_SPACE  = 127,
    ARROW_LEFT  = 1000,
    ARROW_RIGHT ,
    ARROW_UP    ,
    ARROW_DOWN  ,
    DEL_KEY     ,
    HOME_KEY    ,
    END_KEY     ,
    PAGE_UP     ,
    PAGE_DOWN
};

enum editor_highlight {
    HL_NORMAL = 0,
    HL_STRING   ,
    HL_NUMBER   ,
    HL_COMMENT  ,
    HL_MLCOMMENT,
    HL_KEYWORD1 ,
    HL_KEYWORD2 ,
    HL_MATCH
};

// ======================================================================= //
//                               Global Data
// ======================================================================= //

/**
 * @brief 对特定文件类型的语法突出显示信息
 */
typedef struct esyn {
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
} esyn_t;
/** C拓展文件后缀 */
char *C_HL_EXT[] = {".c", ".h", ".cpp", NULL};
/** C拓展文件关键词 */
char *C_HL_KEY[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};
/** 语法突出数据库 */
esyn_t HLDB[] = {
    {
        "c", 
        C_HL_EXT, C_HL_KEY, 
        "//", "/*", "*/",
        HL_SYN_NUMBERS | HL_SYN_STRINGS
    },
};
/** 语法突出数据库大小 */
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/**
 * @brief 编辑器行
 * @note 将一行文本存储为指向动态分配的字符数据的指针和其长度
 */
typedef struct erow {
    /** 文件中自己的索引 */
    int idx;
    /** 动态分配的字符数据的字符串指针 */
    char *c;
    /** 字符串长度 */
    int len;
    /** 指向需要渲染的字符串 */
    char *render;
    /** 渲染内容长度 */
    int rlen;
    /** 语法高亮 */
    unsigned char *hl;
    /** 布尔：高亮是否未闭合 */
    int hl_open_comment;
} erow_t;



/**
 * @brief 编辑器配置结构体，保存了编辑器的信息。
 */
typedef struct editor_config {
    /** 屏幕行数 */
    int screen_rows;
    /** 屏幕列数 */
    int screen_cols;
    /** 光标 x 轴坐标 */
    int cursor_x;
    /** 光标 y 轴坐标 */
    int cursor_y;
    /** 渲染 x 轴坐标 */
    int render_x;
    /** 渲染 y 轴坐标 */
    int render_y;
    /** 编辑器总行数 */
    int num_rows;
    /** 编辑器行偏移量 */
    int row_off;
    /** 编辑器列偏移量 */
    int clo_off;
    /** 脏读标志 */
    int dirty;
    /** 编辑器行 */
    erow_t *row;
    /** 文件名 */
    char *filename;
    /** 状态栏信息 */
    char status_msg[80];
    /** 状态栏信息：时间 */
    time_t status_msg_time;
    /** 语法突出信息 */
    esyn_t *syntax;
    /** 系统终端属性 */
    struct termios orig_termios; 
} editor_config_t;
editor_config_t ec;     /** 全局编辑器配置实例 */

/**
 * @brief 追加缓冲区结构体
 */
typedef struct abuf {
    /** 指向内存中缓冲区的字符串指针 */
    char *b;
    /** 字符串长度 */
    int len;
} abuf_t;

/** 追加缓冲区初始化 */
#define ABUF_INIT {NULL, 0}

/**
 * @brief 缓冲区追加字符串
 * @param ab 追加缓冲区
 * @param s 字符串
 * @param len 字符串长度
 */
void abuf_append(abuf_t *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if(new == NULL)
        return;
    memcpy(&new[ab->len], s, len);      // 新值复制
    ab->b = new;
    ab->len += len;
}

/**
 * @brief 释放追加缓冲区
 * @param ab 追加缓冲区
 */
void abuf_free(abuf_t *ab) {
    free(ab->b);
}


// ======================================================================= //
//                            Func Prototypes
// ======================================================================= //

/**
 * @brief 编辑器设置状态栏消息
 * @param fmt 消息格式化字符串
 * @param ... 其他参数
 */
void editor_set_status_msg(const char *fmt, ...);

/**
 * @brief 编辑器清除屏幕
 */
void editor_refresh_screen();

/**
 * @brief 编辑器显示提示，提供文本输入
 * @param prompt 提示信息
 * @param callback 回调函数
 * @return char* 输入信息
 */
char *editor_prompt(char *prompt, void (*callback)(char *, int));

// ======================================================================= //
//                               Terminal
// ======================================================================= //

/**
 * @brief 错误处理：打印错误信息
 * @param s 错误信息
 */
void fatal(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H" , 3);
    perror(s);
    exit(1);
}

/**
 * @brief 关闭原始文本模式：参考`enable_raw_mode`
 */
void disable_raw_mode() {
    if(tcsetattr(STDIN_FILENO, TCIFLUSH, &ec.orig_termios) == -1)
        fatal("tcsetattr");
}

/**
 * @brief 开启原始文本模式
 * @note 原始文本模式
 * 需要关闭相关功能并获取等待延迟
 * 
 * ## 关闭特定功能
 * 1. `ECHO`功能会将键入打印到终端。
 * 在原始模式下渲染编辑器UI时，
 * 会妨碍我们输入。
 * - 例如，使用`sudo`在终端上输入密码：
 * ```shell
 * sudo apt install xxx
 * ```
 * 2. `ICANON`功能会逐个字节监测键入，
 * 而不是逐行读取，
 * 不需要按下`Enter`键。
 * 3. `ISIG`功能会控制如下信号：
 * 默认情况下，`Ctrl-C`向当前进程发送`SIGINT`信号，
 * 导致其终止；`Ctrl-Z`向当前进程发送`SIGTSTP`信号，
 * 导致其挂起。
 * 4. `IXON`功能会控制如下软件流信号：
 * 默认情况下，`Ctrl-S`停止将数据传输到终端，
 * 直到按`Ctrl-Q`。 
 * 5. `IEXTEN`功能会控制如下信号：
 * 键入`Ctrl-V`时，终端会等待你键入其他字符，
 * 然后发送该字符。关闭`IEXTEN`还可以修复
 * macOS`Ctrl-O`中的问题。
 * 6. `ICRNL`功能控制的`Ctrl-M`很奇怪：
 * 当我们期望它被读取为`13`时，它被读取为`10`
 * 终端有助于将用户输入的任何回车符`（13，\r）`
 * 转换为换行符`（10，\n）`，关闭这个功能。
 * 7. `OPOST`标志控制所有输出处理功能：
 * 在 printf() 语句中添加回车符，
 * 防止光标只向下移动，而不向屏幕的左侧移动。
 * 8. 其他标志：略
 * 
 * ## 超时读取
 * 1. `VMIN`值设置`read()`返回之前所需输入的最小字节数。
 * 我们将其设置为`0`，以便`read()`在有任何输入要读取时立即返回。
 * 2. `VTIME`值设置`read()`返回之前等待的最长时间。
 * 它以十分之一秒为单位，因此我们将其设置为`1/10`秒，即`100`毫秒。
 * 如果`read()`超时，它将返回`0`。这是有道理的，
 * 因为它通常的返回值是读取的字节数。
 */
void enable_raw_mode() {
    if(tcgetattr(STDIN_FILENO, &ec.orig_termios) == -1)            // 读取到`termios`结构体
        fatal("tcgetattr");
    atexit(disable_raw_mode);                                   // 注册程序退出时自动调用的函数
    struct termios raw = ec.orig_termios;                          // 复制结构体
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);   // 手动修改结构体本地标志`c_lflag`
    raw.c_iflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);    
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;        
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)          // 将修改后的结构体传回新的终端属性
        fatal("tcsetattr");
}

/**
 * @brief 编辑器读取键入
 * @return char 键入字符
 * @note Arrow 转义字符处理
 * - 如果我们读取一个转义字符，会立即将另外两个字节读入`seq`缓冲区。
 * 如果其中任何一个读数超时（0.1 秒后），
 * 则假设用户只是按下了 Escape 键并返回该键。
 * 否则，会查看转义序列是否为箭头键转义序列。
 */
int editor_read_key() {
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if(nread == -1 && errno != EAGAIN)
            fatal("read");
    }
    if(c == '\x1b') {        // 键入为转义字符时
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';
        if (seq[0] == '[') {
            if(seq[1] >= '0' && seq[1] <= '9') {
                if(read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
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
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if(seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return 'x1b';
    } else {
        return c;
    }
}

/**
 * @brief 获取光标位置
 * @param rows 行数
 * @param cols 列数
 * @return int 返回值
 * @retval -1 获取失败
 * @retval 0  获取成功
 */
int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) 
        return -1;
    return 0;
}

/**
 * @brief 获取窗口大小
 * @param rows 行数
 * @param cols 列数
 * @return int 返回值
 * @retval -1 获取失败
 * @retval 0  获取成功
 * @note 自定义获取窗口大小
 * - `ioctl()`不能保证能够在所有系统上请求窗口大小，
 * 我们将提供获取窗口大小的方法。
 * - 策略是将光标定位在屏幕的右下角，
 * 然后使用转义序列来查询光标的位置。
 * 这告诉我们屏幕上必须有多少行和列。
 */
int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) 
            return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

// ======================================================================= //
//                              Syntax Highlight
// ======================================================================= //

/**
 * @brief 是否为分隔符
 * @param c 字符
 * @return int 
 */
int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

/**
 * @brief 编辑器更新语法高亮
 * @param row 编辑器行
 */
void editor_update_syntax(erow_t *row) {
    row->hl = realloc(row->hl, row->rlen);
    memset(row->hl, HL_NORMAL, row->rlen);
    if(ec.syntax == NULL) return;

    char **keywords = ec.syntax->keywords;
    char *scs = ec.syntax->singleline_comment_start;
    char *mcs = ec.syntax->multiline_comment_start;
    char *mce = ec.syntax->multiline_comment_end;
    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;
    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && ec.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while(i < row->rlen) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;
        if (scs_len && !in_string && !in_comment) {
            // 处理注释高亮
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->rlen- i);
                break;
            }
        }
        if (mcs_len && mce_len && !in_string) {
            // 处理多行注释高亮
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }
        if (ec.syntax->flags & HL_SYN_STRINGS) {
            // 处理字符串高亮
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rlen) {
                    // 处理转义字符
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }
        if (ec.syntax->flags & HL_SYN_NUMBERS) {
            // 处理数字高亮
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            } // if isdigit
        } // if syntax
        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                i += klen;
                break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }
        prev_sep = is_separator(c);
        i++;
    } // while
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < ec.num_rows)
        editor_update_syntax(&ec.row[row->idx + 1]);
}

/**
 * @brief 编辑器语法转颜色
 * @param hl 语法枚举，参考`dfitor_highlight`
 * @return int 颜色
 */
int editor_syn2col(int hl) {
    switch (hl) {
        case HL_NUMBER   :  return 31;       // red
        case HL_MLCOMMENT:
        case HL_COMMENT  :  return 32;       // green
        case HL_STRING   :  return 33;       // yellow
        case HL_MATCH    :  return 34;       // blue
        case HL_KEYWORD1 :  return 35;       // pink 
        case HL_KEYWORD2 :  return 36;       // cyan
        default: return 37;
    }
}

/**
 * @brief 编辑器匹配文件名的语法高亮
 */
void editor_select_syntax_highlight() {
    ec.syntax = NULL;
    if (ec.filename == NULL)
        return;
    char *ext = strrchr(ec.filename, '.');
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        esyn_t *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(ec.filename, s->filematch[i]))) {
                ec.syntax = s;
                int filerow;
                for (filerow = 0; filerow < ec.num_rows; filerow++) {
                    editor_update_syntax(&ec.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}


// ======================================================================= //
//                            Row Operations
// ======================================================================= //

/**
 * @brief 将字符索引转换为渲染索引
 * @param row 编辑器行
 * @param cx 字符索引
 * @return int 渲染索引
 */
int editor_row_cx2rx(erow_t *row, int cx) {
    int rx = 0;
    int j;
    for(j = 0; j < cx; j++) {
        if(row->c[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        rx++;
    }
    return rx;
}

/**
 * @brief 将渲染索引转换为字符索引
 * @param row 编辑器行
 * @param rx 渲染索引
 * @return int 字符索引
 */
int editor_row_rx2cx(erow_t *row, int rx) {
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->len; cx++) {
        if(row->c[cx] == '\t')
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        cur_rx++;
        if(cur_rx > rx) return cx;
    }
    return cx;
}

/**
 * @brief 编辑器（更新）渲染行
 * @param row 编辑器行
 */
void editor_update_row(erow_t *row) {
    int tabs = 0;
    int j;
    for(j = 0; j < row->len; j++) {
        if(row->c[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->len + tabs*(TAB_STOP - 1) + 1);

    int idx = 0;
    for(j = 0; j < row->len; j++) {
        if(row->c[j] == '\t') {
            row->render[idx++] = ' ';
            while(idx % TAB_STOP != 0) row->render[idx++] = ' ';
        }else{
            row->render[idx++] = row->c[j];
        }
    }
    row->render[idx] = '\0';
    row->rlen = idx;
    editor_update_syntax(row);
}

/**
 * @brief 编辑器加入行
 * @param at 行号
 * @param s 行字符串
 * @param len 行长度
 */
void editor_insert_row(int at, char *s, size_t len) {
    if(at < 0 || at > ec.num_rows) return;
    ec.row = realloc(ec.row, sizeof(erow_t) * (ec.num_rows + 1));
    memmove(&ec.row[at + 1], &ec.row[at], sizeof(erow_t) * (ec.num_rows - at));
    for (int j = at + 1; j <= ec.num_rows; j++) ec.row[j].idx++;

    ec.row[at].idx = at;
    ec.row[at].len = len;
    ec.row[at].c = malloc(len + 1);
    memcpy(ec.row[at].c, s, len);
    ec.row[at].c[len] = '\0';
    ec.row[at].rlen = 0;
    ec.row[at].render = NULL;
    ec.row[at].hl = NULL;
    ec.row[at].hl_open_comment = 0;
    editor_update_row(&ec.row[at]);

    ec.num_rows++;
    ec.dirty++;
}

/**
 * @brief 编辑器释放行
 * @param row 编辑器行
 */
void editor_free_row(erow_t *row) {
    free(row->render);
    free(row->c);
    free(row->hl);
}

/**
 * @brief 编辑器删除行
 * @param at 行号
 */
void editor_del_row(int at) {
    if (at < 0 || at >= ec.num_rows)
        return;
    editor_free_row(&ec.row[at]);
    memmove(&ec.row[at], &ec.row[at + 1], sizeof(erow_t) * (ec.num_rows - at - 1));
    for (int j = at; j < ec.num_rows - 1; j++) ec.row[j].idx--;
    ec.num_rows--;
    ec.dirty++;
}

/**
 * @brief 在给定位置插入单个字符到`erow`
 * @param row 编辑器行
 * @param at 字符索引
 * @param c 字符
 */
void editor_row_insert_char(erow_t *row, int at, int c) {
    if (at < 0 || at > row->len)
        at = row->len;
    row->c = realloc(row->c, row->len + 2);
    memmove(&row->c[at + 1], &row->c[at], row->len - at + 1);
    row->len++;
    row->c[at] = c;
    editor_update_row(row);
    ec.dirty++;
}

/**
 * @brief 编辑器行加入字符串
 * @param row 编辑器行
 * @param s 字符串
 * @param len 字符串长度
 * @note 用于实现删除功能
 */
void editor_row_append_str(erow_t *row, char *s, size_t len) {
    row->c = realloc(row->c, row->len + len + 1);
    memcpy(&row->c[row->len], s, len);
    row->len += len;
    row->c[row->len] = '\0';
    editor_update_row(row);
    ec.dirty++;
}


void editor_row_del_char(erow_t *row, int at) {
    if (at < 0 || at >= row->len)
        return;
    memmove(&row->c[at], &row->c[at + 1], row->len - at);
    row->len--;
    editor_update_row(row);
    ec.dirty++;
}


// ======================================================================= //
//                            Editor Operations
// ======================================================================= //

/**
 * @brief 编辑器插入字符
 * @param c 字符
 */
void editor_insert_char(int c) {
    if(ec.cursor_y == ec.num_rows) {
        editor_insert_row(ec.num_rows, "", 0);
    }
    editor_row_insert_char(&ec.row[ec.cursor_y], ec.cursor_x, c);
    ec.cursor_x++;
}

/**
 * @brief 编辑器插入新行
 */
void editor_insert_newline() {
    if(ec.cursor_x == 0) {
        editor_insert_row(ec.cursor_y, "", 0);
    }else {
        erow_t * row = &ec.row[ec.cursor_y];
        editor_insert_row(ec.cursor_y + 1, &row->c[ec.cursor_x], row->len - ec.cursor_x);
        row = &ec.row[ec.cursor_y];
        row->len = ec.cursor_x;
        row->c[row->len] = '\0';
        editor_update_row(row);
    }
    ec.cursor_y++;
    ec.cursor_x = 0;
}

/**
 * @brief 编辑器删除字符
 */
void editor_del_char() {
    if(ec.cursor_y == ec.num_rows) return;
    if(ec.cursor_x == 0 && ec.cursor_y == 0) return;
    erow_t *row = &ec.row[ec.cursor_y];
    if(ec.cursor_x > 0) {
        editor_row_del_char(row, ec.cursor_x - 1);
        ec.cursor_x--;
    } else {
        ec.cursor_x = ec.row[ec.cursor_y - 1].len;
        editor_row_append_str(&ec.row[ec.cursor_y - 1], row->c, row->len);
        editor_del_row(ec.cursor_y);
        ec.cursor_y--;
    }
}

// ======================================================================= //
//                                File I/O
// ======================================================================= //

/**
 * @brief 编辑器行转化为字符串
 * @param str_len 获取字符串长度
 * @return char* 字符串指针
 */
char* editor_rows2str(int *str_len) {
    int buf_len = 0;
    int j;
    for(j = 0; j < ec.num_rows; j++)
        buf_len += ec.row[j].len + 1;
    *str_len = buf_len;

    char *str = malloc(buf_len);
    char *p = str;
    for(j = 0; j < ec.num_rows; j++) {
        memcpy(p, ec.row[j].c, ec.row[j].len);
        p += ec.row[j].len;
        *p = '\n';
        p++;
    }
    return str;
}

/**
 * @brief 编辑器打开文件
 * @param filename 文件名
 */
void editor_open(char *filename) {
    free(ec.filename);
    ec.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp) fatal("fopen");

    editor_select_syntax_highlight();

    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while((line_len = getline(&line, &line_cap, fp)) != -1){
        while (line_len > 0 && (line[line_len - 1] == '\n' ||
                                line[line_len - 1] == '\r'))
            line_len--;
        editor_insert_row(ec.num_rows, line, line_len);
    }
    free(line);
    fclose(fp);
    ec.dirty = 0;
}

/**
 * @brief 编辑器保存
 */
void editor_save() {
    if(ec.filename == NULL) {
        ec.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (ec.filename == NULL) {
            editor_set_status_msg("Save aborted");
            return;
        }
        editor_select_syntax_highlight();
    }

    int len;
    char *buf = editor_rows2str(&len);
    int fd = open(ec.filename, O_RDWR | O_CREAT, 0644);
    if(fd != -1) {
        if(ftruncate(fd, len) != -1) {
            if(write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                ec.dirty = 0;
                editor_set_status_msg("%d bytes written to disk", len);
                return;
            } // if write
        } // if ftruncate
        close(fd);
    } // if fd
    free(buf);
    editor_set_status_msg("Can't save I/O error: %s", strerror(errno));
}

// ======================================================================= //
//                               Editor Find
// ======================================================================= //

/**
 * @brief 编辑器查找回调函数
 * @param query 查询字符串
 * @param key 键入
 */
void editor_find_callback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;
    static int saved_hl_line;
    static char *saved_hl = NULL;

    if(saved_hl) {
        memcpy(ec.row[saved_hl_line].hl, saved_hl, ec.row[saved_hl_line].rlen);
        free(saved_hl);
        saved_hl = NULL;
    }
    if (key == '\r' || key == '\x1b') {
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
    int current = last_match;
    int i;
    for(i = 0; i < ec.num_rows; i++) {
        current += direction;
        if(current == -1) {
            current = ec.num_rows - 1;
        } else if(current == ec.num_rows) {
            current = 0;
        }

        erow_t *row = &ec.row[current];
        char *match = strstr(row->render, query);
        if(match) {
            last_match = current;
            ec.cursor_y = current;
            ec.cursor_x = editor_row_rx2cx(row, match - row->render);
            // ec.row_off = ec.num_rows;
            saved_hl_line = current;
            saved_hl = malloc(row->rlen);
            memcpy(saved_hl, row->hl, row->rlen);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}


/**
 * @brief 编辑器寻找字符串
 */
void editor_find() {
    int saved_cx = ec.cursor_x;
    int saved_cy = ec.cursor_y;
    int saved_col_off = ec.clo_off;
    int saved_row_off = ec.row_off;
    char *query = editor_prompt("Search: %s (ESC to cancel)", editor_find_callback);
    if(query) {
        free(query);
    } else {
        ec.cursor_x = saved_cx;
        ec.cursor_y = saved_cy;
        ec.clo_off = saved_col_off;
        ec.row_off = saved_row_off;
    }
}

// ======================================================================= //
//                             Screen Output
// ======================================================================= //

/**
 * @brief 编辑器滚动
 * @note 设置`ec.rowoff`值：
 * 策略是检查光标是否已移出可见窗口，
 * 如果是，则调整`ec.rowoff`，
 * 使光标刚好位于可见窗口内。
 */
void editor_scroll() {
    ec.render_x = 0;
    if(ec.cursor_y < ec.num_rows) {
        ec.render_x = editor_row_cx2rx(&ec.row[ec.cursor_y], ec.cursor_x);
    }
    if (ec.cursor_y < ec.row_off) {
        ec.row_off = ec.cursor_y;
    }
    if(ec.cursor_y >= ec.row_off + ec.screen_rows) {
        ec.row_off = ec.cursor_y - ec.screen_rows + 1;
    }
    if(ec.render_x < ec.clo_off) {
        ec.clo_off = ec.render_x;
    }
    if(ec.render_x >= ec.clo_off + ec.screen_cols) {
        ec.clo_off = ec.render_x - ec.screen_cols + 1;
    }
}


/**
 * @brief 编辑器绘制行
 * @param ab 追加缓冲区
 * @note 类似`vim`左侧的波浪。
 * - 我们打印最后一个波形符时，
 * 会像在任何其他行上一样打印“\r\n”，
 * 但这会导致终端滚动以便为新的空白行腾出空间。
 * 故打印“\r\n”时，需要让最后一行成为例外。
 */
void editor_draw_rows(abuf_t *ab) {
    int y;
    for(y = 0; y < ec.screen_rows; y++) {
        int file_row = y + ec.row_off;
        if(file_row >= ec.num_rows) {
            if(ec.num_rows == 0 && y == ec.screen_rows / 3) {
                // 如果新建文件：居中打印欢迎信息    
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                    "texc editor %s", TEXC_VERSION);
                if(welcome_len > ec.screen_cols) welcome_len = ec.screen_cols;
                int padding = (ec.screen_cols - welcome_len) / 2;
                if(padding) {
                    abuf_append(ab, "~", 1);
                    padding--;
                }
                while(padding--) abuf_append(ab, " ", 1);
                abuf_append(ab, welcome, welcome_len);
            } else {
                // 打开旧文件：绘制`~`
                abuf_append(ab, "~",  1);
            } // if y >= ec.num_rows
        } else {
            // 绘制文件内字符串
            int len = ec.row[file_row].rlen - ec.clo_off;
            if(len < 0) len = 0;
            if(len > ec.screen_cols) len = ec.screen_cols;
            char *c = &ec.row[file_row].render[ec.clo_off];
            unsigned char *hl = &ec.row[file_row].hl[ec.clo_off];
            int current_color = -1;
            int j;
            for(j = 0; j < len; j++) {
                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abuf_append(ab, "\x1b[7m", 4);
                    abuf_append(ab, &sym, 1);
                    abuf_append(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abuf_append(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if(current_color != -1) {
                        abuf_append(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abuf_append(ab, &c[j], 1);
                } else {
                    int color = editor_syn2col(hl[j]);
                    if(color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abuf_append(ab, buf, clen);
                    }
                    abuf_append(ab, &c[j], 1);
                } // if isdigit
            } // for j
            abuf_append(ab, "\x1b[39m", 5);
        }
        // 擦除光标右侧部分
        abuf_append(ab, "\x1b[K", 3);       
        abuf_append(ab, "\r\n", 2);
    } // for y
}

/**
 * @brief 编辑器绘制状态栏
 * @param ab 追加缓冲区
 */
void editor_draw_status_bar(abuf_t *ab) {
    abuf_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        ec.filename ? ec.filename : "[No Name]", ec.num_rows,
        ec.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        ec.syntax ? ec.syntax->filetype : "NA", ec.cursor_y + 1, ec.num_rows);
    if(len > ec.screen_cols) len = ec.screen_cols;
    abuf_append(ab, status, len);
    while(len < ec.screen_cols) {
        if(ec.screen_cols - len == rlen) {
            abuf_append(ab, rstatus, rlen);
            break;
        } else{
            abuf_append(ab, " ", 1);
            len++;
        } // if
    } // while
    abuf_append(ab, "\x1b[m", 3);
    abuf_append(ab, "\r\n", 2);
}

/**
 * @brief 编辑器绘制状态栏消息
 * @param ab 追加缓冲区
 */
void editor_draw_status_msg(abuf_t *ab) {
    abuf_append(ab, "\x1b[K", 3);
    int msg_len = strlen(ec.status_msg);
    if(msg_len > ec.screen_cols) msg_len = ec.screen_cols;
    if(msg_len && time(NULL) - ec.status_msg_time < 5)
        abuf_append(ab, ec.status_msg, msg_len);
}

/**
 * @brief 编辑器设置状态栏消息
 * @param fmt 消息格式化字符串
 * @param ... 
 */
void editor_set_status_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ec.status_msg, sizeof(ec.status_msg), fmt, ap);
    va_end(ap);
    ec.status_msg_time = time(NULL);
}

/**
 * @brief 编辑器清除屏幕
 */
void editor_refresh_screen() {
    editor_scroll();
    abuf_t ab = ABUF_INIT;
    abuf_append(&ab, "\x1b[?25l", 6);       // 处理光标闪烁
    abuf_append(&ab, "\x1b[H"   , 3);       // 放置光标左上角
    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_status_msg(&ab);
    
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (ec.cursor_y - ec.row_off) + 1, 
                                              (ec.render_x - ec.clo_off) + 1);
    abuf_append(&ab, buf, strlen(buf));     // 放置光标到 (x, y)

    abuf_append(&ab, "\x1b[?25h", 6);
    write(STDOUT_FILENO, ab.b, ab.len);
    abuf_free(&ab);
}




// ======================================================================= //
//                             Keyboard Input
// ======================================================================= //

/**
 * @brief 编辑器显示提示，提供文本输入
 * @param prompt 提示信息
 * @param callback 回调函数
 * @return char* 输入信息
 */
char *editor_prompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';
    while(1) {
        editor_set_status_msg(prompt, buf);
        editor_refresh_screen();
        int c = editor_read_key();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACK_SPACE) {
            if(buflen != 0) buf[--buflen] = '\0';
        } else if(c == '\x1b') {
            editor_set_status_msg("");
            if(callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if(c == '\r') {
            if(buflen != 0) {
                editor_set_status_msg("");
                if(callback) callback(buf, c);
                return buf;
            }
        } else if(!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        } // if c
        if(callback) callback(buf, c);
    } // while 1
}

/**
 * @brief 编辑器移动光标位置
 * @param key 键入字符
 */
void editor_move_cursor(int key) {
    erow_t *row = (ec.cursor_y >= ec.num_rows) ? NULL : &ec.row[ec.cursor_y];
    switch (key){
    case ARROW_LEFT:
        if(ec.cursor_x != 0) { 
            ec.cursor_x--;
        } else if(ec.cursor_y > 0) {
            // 允许左移到上一行末尾
            ec.cursor_y--;
            ec.cursor_x = ec.row[ec.cursor_y].len;
        }
        break;
    case ARROW_RIGHT:
        if(row && ec.cursor_x < row->len) {
            ec.cursor_x++;
        } else if(row && ec.cursor_x == row->len) {
            // 允许右移到下一行开头
            ec.cursor_y++;
            ec.cursor_x = 0;
        }
        break;
    case ARROW_UP:
        if (ec.cursor_y != 0) ec.cursor_y--;
        break;
    case ARROW_DOWN:
        if (ec.cursor_y < ec.num_rows) ec.cursor_y++;
        break;
    default:
        break;
    }
    // 将光标对齐到行尾
    row = (ec.cursor_y >= ec.num_rows) ? NULL : &ec.row[ec.cursor_y];
    int row_len = row ? row->len : 0;
    if(ec.cursor_x > row_len) ec.cursor_x = row_len;
}


/**
 * @brief 编辑器处理键入
 */
void editor_proc_key() {
    static int quit_times = QUIT_TIMES;
    int c = editor_read_key();
    switch (c) {
    case '\r':
        editor_insert_newline();
        break;
    case CTRL_KEY('q'):
        if(ec.dirty && quit_times > 0) {
            editor_set_status_msg("WARN: File has changes. "
            "Press Ctrl-Q %d more times to unsaved quit.", quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);         // 清除屏幕
        write(STDOUT_FILENO, "\x1b[H" , 3);         // 定位左上角
        exit(0);
        break;
    case CTRL_KEY('s'):
        editor_save();
        break;
    case CTRL_KEY('f'):
        editor_find();
        break;
    case CTRL_KEY('l'):
    case '\x1b':
        /// TODO: 处理特殊字符
        break;
    case BACK_SPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        {   // 删除字符
            if(c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
            editor_del_char();
            break;
        }
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editor_move_cursor(c);
        break;
    case HOME_KEY:
        ec.cursor_x = 0;
        break;
    case END_KEY:
        // 移动到当前行的尾行
        if (ec.cursor_y < ec.num_rows)
            ec.cursor_x = ec.row[ec.cursor_y].len;
        break;
    case PAGE_UP:
    case PAGE_DOWN:
        {
            if(c == PAGE_UP) {
                ec.cursor_y = ec.row_off;
            } else if (c == PAGE_DOWN) {
                ec.cursor_y = ec.row_off + ec.screen_rows - 1;
                if (ec.cursor_y > ec.num_rows) ec.cursor_y = ec.num_rows;
            }
            int times = ec.screen_rows;
            while(times--)
                editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
        break;
    default:
        editor_insert_char(c);
        break;
    }
    quit_times = QUIT_TIMES;
}

// ======================================================================= //
//                                Init
// ======================================================================= //

/**
 * @brief 编辑器初始化
 */
void editor_init() {
    ec.cursor_x = 0;
    ec.cursor_y = 0;
    ec.render_x = 0;
    ec.render_y = 0;
    ec.num_rows = 0;
    ec.row_off  = 0;
    ec.clo_off  = 0;
    ec.dirty    = 0;
    ec.row      = NULL;
    ec.filename = NULL;
    ec.syntax   = NULL;
    ec.status_msg[0] = '\0';
    ec.status_msg_time = 0;
    if(get_window_size(&ec.screen_rows, &ec.screen_cols) == -1)
        fatal("get_window_size");
    ec.screen_rows -= 2;
}


// ======================================================================= //
//                              Proc Entry
// ======================================================================= //

int main(int argc, char* argv[]) {
    enable_raw_mode();
    editor_init();
    if(argc >= 2) {
        editor_open(argv[1]);
    }
    editor_set_status_msg("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");
    while(1) {
        editor_refresh_screen();
        editor_proc_key();
    }
    return 0;
}
