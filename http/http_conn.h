#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
class http_conn
{
public:
    static const int FILENAME_LEN = 200;

    // static const int READ_BUFFER_SIZE = 20480000;
    // static const int WRITE_BUFFER_SIZE = 20480000;
    static const int READ_BUFFER_SIZE = 102400000;
    static const int WRITE_BUFFER_SIZE = 102400000;

        // static const int READ_BUFFER_SIZE = 50;
        // static const int WRITE_BUFFER_SIZE = 50;
    

    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST, // 请求不完整，还需继续解析
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION,
        FILE_DOWNLOAD       // 增加文件下载状态码
    };
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    // ~http_conn() {}
    ~http_conn() {
        delete [] m_read_buf;
        delete [] m_write_buf;
    }

public:
    void init(int sockfd, const sockaddr_in &addr);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    void initmysql_result(connection_pool *connPool);

private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

    // 当 状态码为 下载文件时，下载文件需要添加一些特殊的 请求头部
    bool add_download_headers(int content_length, const char * filename); 
    bool add_download_content_type();
    bool add_content_disposition(const char * filename);

public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;

private:
    int m_sockfd;
    sockaddr_in m_address;

    // char m_read_buf[READ_BUFFER_SIZE];
    char * m_read_buf = new char[READ_BUFFER_SIZE];

    int m_read_idx;
    int m_checked_idx;
    int m_start_line;

    // char m_write_buf[WRITE_BUFFER_SIZE];
    char * m_write_buf = new char[WRITE_BUFFER_SIZE];

    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN]; // 存放请求目标文件的整个路径，如 /home/fd/vscode_workspace/tinywebserver_raw_version/root/index.html
    char *m_url;  //请求目标文件的文件名, 如 /index.html
    char *m_version;
    char *m_host;
    int m_content_length;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求体数据，POST请求中最后为输入的用户名和密码，存储至 m_string 中，格式为 user=abc&password=123456
    int bytes_to_send;
    int bytes_have_send;

    // 自己新增变量

    char entername[100]; // 登录名记录下来，当上传和下载文件时，用于和数据库中的名字进行验证
    char * boundary;

    int tag; // 用于传送长文件而在 parse_content 中设置的变量，在 init 中初始化。当此变量 在 parse_content 中设置为 1 时，parse_line 函数中便不会在 for 循环中改变 m_checked_idx;
                 // 在 parse_content 函数中 if (m_read_idx >= (m_content_length + m_checked_idx)) 判断中将该变量重新置为 0
    char downloadname[500]; // 要下载文件的文件名（不包含路径），用于在 add_download_headers 中做参数，在 do_request 中被赋值
};

#endif
