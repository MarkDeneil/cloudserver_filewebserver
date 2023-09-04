#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>
#include <iostream>

// #define connfdET //边缘触发非阻塞
#define connfdLT //水平触发阻塞

// #define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/fd/vscode_workspace/file_webserver/root";

//将表中的用户名和密码放入map
map<string, string> users;
locker m_lock;

// 将系统文件名表中的文件名和用户名放入
map<string, string> filetable;

// 用户文件名表
map<string, string> userfiletable;


void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        // LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }

    // for (map<string, string>::iterator it = users.begin(); it != users.end(); ++it) {
    //     std::cout << (*it).first << " " << (*it).second << std::endl;
    // }


    // 系统文件名和用户名存入 map 中，这部分文件不应该被修改

     if (mysql_query(mysql, "SELECT filename,username FROM filetable"))
    {
        // LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    result = mysql_store_result(mysql);

    //返回结果集中的列数
    num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        filetable[temp1] = temp2;
    }

    // for (map<string, string>::iterator it = filetable.begin(); it != filetable.end(); ++it) {
    //     std::cout << (*it).first << " " << (*it).second << std::endl;
    // }

    // 用户文件表存入 userfiletable 中

     if (mysql_query(mysql, "SELECT filename,username FROM userfiletable"))
    {
        // LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    result = mysql_store_result(mysql);

    //返回结果集中的列数
    num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        userfiletable[temp1] = temp2;
    }

    // for (map<string, string>::iterator it = userfiletable.begin(); it != userfiletable.end(); ++it) {
    //     std::cout << (*it).first << " " << (*it).second << std::endl;
    // }

}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);


    // 自己改动的新变量初始化
    memset(entername, '\0', 100);
    boundary = 0;
    tag = 0;
    memset(downloadname, '\0', 500);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    
    // std::cout << "parse_line 1" << std::endl;
    char temp;
    // cout << "m_checked_idx:" << m_checked_idx << endl;
    // cout << "m_read_idx:" << m_read_idx << endl;

    if (m_check_state == CHECK_STATE_CONTENT && tag == 1) { // 为处理大文件传输而设置变量 tag 为 1，在这种情况下不会进入下面的 for 循环改变 m_checked_idx
        return LINE_OPEN; 
    }

    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            // cout << "parse_line 2 r" << endl;
            if ((m_checked_idx + 1) == m_read_idx) 
                return LINE_OPEN;                   // 这种情况下，parse_line()会返回 LINE_OPEN，而调用 parse_line() 的函数 process_read 会返回 NO_REQUEST，调用 process_read 的
                                                // process() 函数在收到返回值 NO_REQUEST 后，会重置该 http socket 的 EOPLLIN 并退出，但注意，该 http 对象并没有销毁，主线程中继续
                                                // 调用 read_once 函数将数据读取到 m_read_buf+m_read_idx 处，然后会再次运行到此处，此时的 m_checked_idx 还是上次退出该函数时的值
                                                // （因为 http 对象是在主线程中定义的，并传给了工作线程 http 对象的地址），因此此次调用到此处，m_checked_idx + 1 不会等于
                                                //  m_read_idx 了
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                // cout << "parse_line 2 n" << endl;
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            // cout << "parse_line 3 n" << endl;
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                // cout << "parse_line 3 r" << endl;
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;                           // 既没有读取到 /r，也没有读取到 /n，则返回 LINE_OPEN
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

#ifdef connfdLT

    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
    m_read_idx += bytes_read;

    if (bytes_read <= 0)
    {
        return false;
    }

    return true;

#endif

#ifdef connfdET
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        // std::cout << "yes3" << endl;
        if (bytes_read == -1)
        {
            // std::cout << "yes4" << endl;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // std::cout << "yes5" << endl;
                break;
            }
            return false;
        }
        else if (bytes_read == 0)
        {
            // std::cout << "yes6" << endl;
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // std::cout << "parse_request_line 1" << std::endl;

    // GET /index.html HTTP/1.1  一个请求行
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;                         // 如果返回 BAD_REQUEST，则调用 parse_request_line 的 process_read 会直接返回 BAD_REQUEST。之后该返回值会作为 process_write
                                                    // 的参数，process_write 会根据该 BAD_REQUEST 返回值调用 add_status_line(404, error_404_title); add_headers(strlen(error_404_form));
                                                    // 以及 add_content(error_404_form) 在 m_write_buf 中构造应答报文
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;    // 指向 http:// 之后的第一个字符
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url只为 "/" 时，自动加上判断界面文件名,这种情况发生于请求行是类似 GET / HTTP/1.1
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;                      // parse_request_line 返回 NO_REQUEST 并将 m_check_state 置为 CHECK_STATE_HEADER
                                            // 返回 NO_REQUEST 后，调用 parse_request_line 的 process_read 会判断是否返回值为 BAD_REQUEST，如果是，则 process_read 函数返回 BAD_REQUEST
                                            // 如果不是，则 跳出 switch (m_check_state) 开关语句，并紧接着继续进行循环：
                                            // while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
                                            // 显然会接着使用 parse_line() 解析一行，并且下次循环会进入开关语句的 m_check_state 的 CHECK_STATE_HEADER 部分，即处理请求头
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    std::cout << "parse_headers 1" << std::endl;
    if (text[0] == '\0')                            // 当进入循环条件中的 parse_line 遇到一个 /r/n 空行时，会直接将这两个字符变为 "\0\0"并将 m_checked_idx 置为下一个字符处，即请求体的第一个字符
                                                    // 进入循环后，使用 get_line 获取 "\0\0" 并赋给变量 text，并将 m_checked_idx 赋给 m_start_line，即请求体的第一个字符。之后因为 m_chenk_state
                                                    // 为 CHECK_STATE_HEADER，进入开关语句的 parse_headers，此时会进入此处的判断条件，即 text[0] == '\0'
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;                      // 如果 m_content_length不为0，parse_headers 将 m_check_state 置为 CHECK_STATE_CONTENT，并返回 NO_REQUEST。之后 process_read
                                                    // 会判断返回值是否为 BAD_REQUEST 或 GET_REQUEST，如果不是，则跳出 switch 开关语句，并接着进行循环：
                                                    // while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
                                                    // 之后会根据第一个条件，m_check_state == CHECK_STATE_CONTENT 且 line_status 是上一次进入循环时的 parse_line 的返回值 LINE_OK，
                                                    // 进入循环并进入开关语句的 m_check_state 的 CHECK_STATE_CONTENT 部分，处理请求体
                                                    
                                                    // 这里注意，第一次进入请求体 parse_content 函数时，会根据 while 循环语句的第一个条件进入 parse_content 函数，不会执行 parse_line 函数，
                                                    // 此时 m_start_line 和 m_checked_idx 的位置都不会变。即第一次进入 parse_content 函数时，m_checked_idx 和 m_start_line 都指向
                                                    // "\0\0" 之后的第一个字符，即 http 报文请求体的第一个字符
        }
        return GET_REQUEST;                         // 如果 m_content_length 为0，说明该请求报文没有请求体，直接返回 GET_REQUEST。process_read 函数接收到返回值 GET_REQUEST后，执行
                                                    // do_request 函数
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Content-Type: multipart/form-data; ", 35) == 0)
    {
        // std::cout << text << std::endl;
        text += 35;
        text += strspn(text, " \t");
        text += 9;
        // cout << strlen(text) << endl;
        // std::cout << text << std::endl;
        boundary = text;
        
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        // LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;                              // 如果这里返回 NO_REQUEST，则返回后 process_read 函数会判断返回值是否为 BAD_REQUEST 或 GET_REQUEST，如果不是，则跳出开关语句，
                                                    // 并继续进入下一次循环
                                                    // while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
                                                    // 之后会继续根据第二个条件，根据 parse_line 的返回值继续进入该循环（此时 m_check_state 仍为 CHECK_STATE_HEADER，即进入循环后
                                                    // 仍然进入开关语句中处理请求头 parse_header 继续处理使用 parse_line 新解析的一行
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{

    std::cout << "parse_content 1" << std::endl;
    cout << "m_content_length : " << m_content_length << endl;
    cout << "m_read_idx" << m_read_idx << endl;
    cout << "m_checked_idx" << m_checked_idx << endl;
    if (m_read_idx >= (m_content_length + m_checked_idx))           // 由 parse_header 中的分析，第一次进入 parse_content 时，是根据 while 循环的第一个条件来进入的，
                                                                    // 此时m_checked_idx 和 m_start_line 都指向 "\0\0" 之后的第一个字符，即 http 报文请求体的第一个字符。
                                                                    // 之后将 m_read_buf + m_start_line 赋给 text，并令 m_start_line = m_checked_idx。此时 m_start_line 和 m_checked_idx 仍都指向"\0\0"之后的第一个位置（因为刚刚进入循环时没有进入 parse_line 函数，因此 m_checked_idx 的位置没有变化）
                                                                    // 此时比较 m_read_idx 和 m_content_length + m_checked_idx，
                                                                    // 如果 m_read_idx >= m_content_length + m_checked_idx，说明 m_read_buf 中已经存入了一整个请求体的内容，
                                                                    // 则将 text 存入 m_string 中，并返回 GET_REQUEST。process_read 接收到返回值 GET_REQUEST 后，执行 do_request
    {
        cout << "parse_content in if" << endl;
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码，存储至 m_string 中,格式为 user=abc&password=123456
        m_string = text;

        std::cout << "m_content_length:" << m_content_length << endl;

        // std::cout << strlen(text) << std::endl;
        // for (int i = 0; i < m_content_length; ++i) {
        //     std::cout << i << ":" << text[i] << std::endl;
        // }

        // std::cout << strlen(m_string) << std::endl;
        // for (int i = 0; i < m_content_length; ++i) {
        //     std::cout << i << ":" << m_string[i] << std::endl;
        // }

        // std::cout << strlen(text) << std::endl;
        // std::cout << text << std::endl;
        // std::cout << "strlen(m_string):" << strlen(m_string) << std::endl;
        // std::cout << m_string << std::endl;
        

        // 将处理大文件传输变量 tag 置为 0
        tag = 0;

        return GET_REQUEST;
    }

    else { // 为传送大文件而添加的修改
        tag = 1;
    }

    return NO_REQUEST;                  // 如果在上文的 if 语句比较中， m_read_idx < m_content_length + m_checked_idx，说明此时 m_read_buf 中 m_checked_idx 之后的内容不是
                                        // 整个请求体的内容，此时会返回 NO_REQUEST，此时 process_read 中会判断是否返回 GET_REQUEST，如果是，则执行 do_request
                                        // 如果不是，则将 line_status 置为 LINE_OPEN，之后退出开关语句，继续判断 while 循环：
                                        // while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
                                        // 此时 m_check_state 是 CHECK_STATE_CONTENT ，但 line_status 不为 LINE_OK，则进行第二个条件判断，进入 parse_line 函数
                                        // 进入 parse_line 函数后，执行 for 循环判断 m_read_buf[m_checked_idx] 是否为 '\r' 或者 '\n'，直到 m_checked_idx 为 m_read_idx（注意 m_start_line 仍然未变，仍然指向"\0\0" 之后的第一个位置，之后会用到）
                                        // 之后一直不会读取到 '\r' 或者 '\n'，则返回 LINE_OPEN，process_read 接收到返回值 LINE_OPEN 后，会退出 while 循环，返回 NO_REQUEST
                                        // process 函数接收到返回值 NO_REQUEST 后，会设置该 socket 上的 EPOLLIN 事件并返回。
                                        // 之后主线程第二次调用 read_once 函数，将数据读到 m_read_buf + m_read_idx 处，紧接着调用 process函数，process 函数调用 process_read 函数，
                                        // process_read 函数初始化 line_statue 为 LINE_OK，
                                        // （但 m_check_state 仍为上一次执行 process_read 时设置的 CHECK_STATE_CONTENT，同样的，m_checked_idx 指向第二次调用 read_once 之前的
                                        //  m_read_idex 处，实际上就是第一次读取到的不完整的请求体的最后一个位置，m_start_line 仍然指向"\0\0" 之后的第一个位置）
                                        // 此时判断 while 循环的第一个条件，满足并进入循环，使用 get_line 函数将自 m_read_buf + m_start_line 开始的之后的所有内容（即第一次读取到的
                                        // 不完整的请求体加上第二次调用 read_once 读取到的新的请求体）赋给 text，执行 m_start_line = m_checked_idx 并进入 parse_content 判断
}

//
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    
    // std::cout << "process_read 1" << std::endl;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK)) 
                                                                                            // parse_line 会将 /r/n 置为 \0，并将 m_checked_idx 置为 \0 的后一个字符
    {
        text = get_line();          // return m_read_buf + m_start_line;
        m_start_line = m_checked_idx;
        // LOG_INFO("%s", text);
        Log::get_instance()->flush();
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {

            ret = parse_request_line(text); // 返回 HTTP_CODE
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()            // 进入到该函数时，http 报文已经解析完毕。m_string 中若有内容，则说明有请求体，否则说明没有请求体
{
    // std::cout << "do_request 1" << std::endl;


    strcpy(m_real_file, doc_root);          // m_real_file 中存放最终要访问的文件名
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');


    // 当请求头为                        应给出的界面
    // POST /0 HTTP/1.1                 注册界面
    // POST /1 HTTP/1.1                 登录界面
    // POST /2CGISQL.cgi HTTP/1.1       登录后返回的主界面，即选择界面，或登录失败界面
    // POST /3CGISQL.cgi HTTP/1.1       注册完成成功后的登录界面，或注册失败界面



    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i) {
            if (i > 104)
                return BAD_REQUEST;         // 防止用户名过长
            name[i - 5] = m_string[i];
        }
            
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j) {
            if (j > 99)
                return BAD_REQUEST;        // 防止密码过长
            password[j] = m_string[i];
        }
        password[j] = '\0';

        //同步线程登录校验
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，就把用户名和密码添加进数据库中
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {

                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res) // mysql_query 成功返回 0
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password) {
                strcpy(m_url, "/welcome.html");
                strcpy(entername, name);
                // std::cout << name << "  " << entername << std::endl;
            }
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == 'f') {  // 处理welcome 页面中文件点击操作
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/file.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == 'u') { // 处理上传的文件
        cout << "ufile" << endl;
        
        char * newfilename = NULL; // 指向 m_string 中新的文件名的指针
        char * fileincontent = m_string; // 真正的文件内容在 m_string 中的指针
        if (fileincontent == NULL)
            return INTERNAL_ERROR;
        int fileincontentlen = m_content_length; // 真正的文件内容的长度

        fileincontent += 2 + strlen(boundary) + 2; // "--" + boundary + "\r\n"
        fileincontentlen -= (2 + strlen(boundary) + 2);

        newfilename = strstr(fileincontent, "filename");
        if (newfilename == NULL)
            return INTERNAL_ERROR;
        newfilename += 10; // filename=" 

        int newfilenamelen = strchr(newfilename, '"') - newfilename; // 新的文件名的长度

        if (newfilenamelen == 0) {
            cout << "文件名长度为 0" << endl;
            char *m_url_real = (char *)malloc(sizeof(char) * 200);
            strcpy(m_url_real, "/fileError.html");
            strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
            free(m_url_real);

        } else {
            // for (int i = 0; i < newfilenamelen; ++i) {
            //     cout << i << ":" << newfilename[i] << endl;
            // }

            int doc_root_len = strlen(doc_root);
            char newfilenamearr[doc_root_len + newfilenamelen + 1]; // newfilenamearr 用于存放新的文件名的整体路径+文件名
            char newfilenameonlyarr[newfilenamelen]; // newfilenameonlyarr 用于仅存放新的文件名
            

            strcpy(newfilenamearr, doc_root);
            newfilenamearr[doc_root_len] = '/';
            for (int i = doc_root_len + 1; i < doc_root_len + 1 + newfilenamelen; ++i) {
                newfilenamearr[i] = newfilename[i - (doc_root_len + 1)];
            }
            newfilenamearr[doc_root_len + newfilenamelen + 1] = '\0';

            for (int i = 0; i < newfilenamelen; ++i) {
                newfilenameonlyarr[i] = newfilename[i];
            }
            newfilenameonlyarr[newfilenamelen] = '\0';

            cout << newfilenamearr << endl;
            cout << newfilenameonlyarr << endl;

            if (filetable.find(newfilenameonlyarr) != filetable.end()) {

                cout << "文件名与系统文件名冲突" << endl;
                char *m_url_real = (char *)malloc(sizeof(char) * 200);
                strcpy(m_url_real, "/fileError.html");
                strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
                free(m_url_real);

            } else {
                char * tmpfileincontent = strstr(newfilename, "\r\n\r\n");
                if (tmpfileincontent == NULL)
                    return INTERNAL_ERROR;
                tmpfileincontent += 4;
                fileincontentlen -= (tmpfileincontent - fileincontent);

                fileincontent = tmpfileincontent;
                fileincontentlen -= 2; // "--" + boundary + "--\r\n" 之前还有一个 "\r\n"
                fileincontentlen -= (2 + strlen(boundary) + 4); // "--" + boundary + "--\r\n"

                // for (int i = 0; i < fileincontentlen; ++i) {
                //     cout << i << ":" << fileincontent[i] << endl;
                // }

                int fd = open(newfilenamearr, O_RDWR | O_CREAT, 0777);
                if(fd == -1) {
                    perror("open");
                    return INTERNAL_ERROR;              // 文件名过长时，会在这里出错
                }

                truncate(newfilenamearr, fileincontentlen); // 对新创建的文件进行拓展

                void * ptr = mmap(NULL, fileincontentlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

                if(ptr == MAP_FAILED) {
                    perror("mmap");
                    return INTERNAL_ERROR;
                }

                memcpy(ptr, (void*)fileincontent, fileincontentlen);
                munmap(ptr, fileincontentlen);
                close(fd);

                cout << "上传文件成功" << endl;
                char *m_url_real = (char *)malloc(sizeof(char) * 200);
                strcpy(m_url_real, "/fileSuccess.html");
                strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
                free(m_url_real);

            }
        
        }

    } else if (*(p + 1) == 'd') {       // 处理下载文件请求
        // 此时 m_string 中内容类似：filename=111.txt  ，注意文件名两边没有双引号
        cout << "dfile" << endl;
        char * newfilename = NULL; // 指向 m_string 中下载文件名的指针
        int newfilenamelen = 0; // 下载文件名的长度
        newfilename = m_string;
        newfilename += 9; // filename=
        char * tmp = strchr(newfilename, '%');
        if (tmp != NULL) {
            cout << tmp << endl;
            cout << "文件名中不能含有 #,%,&,+,/,\\,=,?,空格,.,: 等特殊字符" << endl;
            char *m_url_real = (char *)malloc(sizeof(char) * 200);
            strcpy(m_url_real, "/fileError_download.html");
            strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
            free(m_url_real);
        } else {
            newfilenamelen = m_content_length - 9; // m_content_length 减去 "filename=" 这九个字符的长度
            cout << "newfilenamelen:" << newfilenamelen << endl;
            if (newfilenamelen == 0) {
                cout << "文件名长度为 0" << endl;
                char *m_url_real = (char *)malloc(sizeof(char) * 200);
                strcpy(m_url_real, "/fileError_download.html");
                strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
                free(m_url_real);
            } else {
                // for (int i = 0; i < newfilenamelen; ++i) {
                //     cout << i << ":" << newfilename[i] << endl;
                // }

                int doc_root_len = strlen(doc_root);
                char newfilenamearr[doc_root_len + newfilenamelen + 1]; // newfilenamearr 用于存放新的文件名的整体路径+文件名
                char newfilenameonlyarr[newfilenamelen]; // newfilenameonlyarr 用于仅存放新的文件名
                

                strcpy(newfilenamearr, doc_root);
                newfilenamearr[doc_root_len] = '/';
                for (int i = doc_root_len + 1; i < doc_root_len + 1 + newfilenamelen; ++i) {
                    newfilenamearr[i] = newfilename[i - (doc_root_len + 1)];
                }
                newfilenamearr[doc_root_len + newfilenamelen + 1] = '\0';

                for (int i = 0; i < newfilenamelen; ++i) {
                    newfilenameonlyarr[i] = newfilename[i];
                }
                newfilenameonlyarr[newfilenamelen] = '\0';

                cout << "newfilnamearr:" << newfilenamearr << endl;
                cout << "newfilenameonlyarr:" << newfilenameonlyarr << endl;

                if (filetable.find(newfilenameonlyarr) != filetable.end()) {

                    cout << "文件名与系统文件名冲突" << endl;
                    char *m_url_real = (char *)malloc(sizeof(char) * 200);
                    strcpy(m_url_real, "/fileError_download.html");
                    strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
                    free(m_url_real);

                } else {
                    // struct stat tmp_file_stat;
                    if (stat(newfilenamearr, &m_file_stat) < 0 || (!(m_file_stat.st_mode & S_IROTH)) || (S_ISDIR(m_file_stat.st_mode))) {

                        cout << "无该文件 或 权限不足 或 是目录" << endl;
                        char *m_url_real = (char *)malloc(sizeof(char) * 200);
                        strcpy(m_url_real, "/fileError_download.html");
                        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
                        free(m_url_real);

                    } else {
                        
                        int fd = open(newfilenamearr, O_RDONLY);
                        m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

                        close(fd);
                        strcpy(downloadname, newfilenameonlyarr);
                        cout << "downloadfile 结束" << endl;
                        return FILE_DOWNLOAD;
                    }
                }
            }

        }

    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    

    // printf("%s\n", m_url);


    if (stat(m_real_file, &m_file_stat) < 0) {
        // cout << "do_request 1" << endl;
        return NO_RESOURCE;

    }
        
    if (!(m_file_stat.st_mode & S_IROTH)){
        // cout << "do_request 2" << endl;
        return FORBIDDEN_REQUEST;
    }
        
    if (S_ISDIR(m_file_stat.st_mode)){
        // cout << "do_request 3" << endl;
        return BAD_REQUEST;
    }
        
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    // cout << "do_request 4" << endl;
    // printf("%s\n", m_file_address);

    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        // cout << "unmap\n" << endl;
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();                             // 如果 bytes_to_send == 0，重置所有参数，如 m_start_line,m_read_idx,m_checked_idx 等
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0) 
        {
            if (errno == EAGAIN) 
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);  // 非阻塞写，返回 -1 时若 errno 为 EAGAIN 认为连接是正常的，则继续发送
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)  // m_iv[0] 是 m_write_buf 缓冲区
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger) // http 是否保持长连接，如果保持长连接，则发送完响应报文后，初始化 m_start_line,m_read_idx,m_checked_idx 等参数，并返回 true
                          // 主线程中在接收到 true 返回值后，会延长定时器事件
            {
                init();
                return true;
            }
            else            // 如果不抱持长连接，则发送完响应报文后，返回 false，从定时器中删除该 http 对象对应的定时器
            {
                return false;
            }
        }
    }
}

bool http_conn::add_response(const char *format, ...)
{
    // cout << "add_response1" << endl;
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    // cout << "add_response2" << endl;
    m_write_idx += len;
    va_end(arg_list);
    // cout << "add_response3" << endl;
    // LOG_INFO("request:%s", m_write_buf);
    // cout << "add_response4" << endl;
    Log::get_instance()->flush();
    
    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type: %s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 服务下载文件新添加的函数

bool http_conn::add_download_headers(int content_len, const char * filename) {

        add_content_length(content_len);
        add_download_content_type();
        add_content_disposition(filename);
        add_linger();
        add_blank_line();
}

bool http_conn::add_download_content_type() {
    return add_response("Content-Type: %s\r\n", "application/octet-stream");
}

bool http_conn::add_content_disposition(const char * filenmae) {
    return add_response("Content-Disposition: attachment; filename=\"%s\"\r\n", filenmae);
}


bool http_conn::process_write(HTTP_CODE ret)
{
    // cout << "process_write 1" << endl;
    switch (ret)
    {
        case INTERNAL_ERROR:
        {
            // cout << "process_write 1.1" << endl;
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form))
                return false;
            break;
        }
        case BAD_REQUEST:
        {
            // cout << "process_write 1.2" << endl;
            add_status_line(404, error_404_title);
            // cout << "process_write 1.2.1" << endl;
            add_headers(strlen(error_404_form));
            // cout << "process_write 1.2.2" << endl;
            if (!add_content(error_404_form))
                return false;
            // cout << "process_write 1.2.3" << endl;
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            // cout << "process_write 1.3" << endl;
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form))
                return false;
            break;
        }
        case FILE_REQUEST:
        {
            // cout << "process_write 1.4" << endl;
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size != 0)
            {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        case FILE_DOWNLOAD:
        {
            add_status_line(200, ok_200_title);
            add_download_headers(m_file_stat.st_size, downloadname);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        default:
            return false;
    }
    // cout << "process_write 2" << endl;
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    // std::cout << "process 节点1" << std::endl;
    HTTP_CODE read_ret = process_read();
    // std::cout << "process 节点2" << std::endl;
    if (read_ret == NO_REQUEST)
    {
        // std::cout << "process 节点3" << std::endl;
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        // std::cout << "process 节点3.1" << std::endl;
        return;
    }
    // std::cout << "process 节点4" << std::endl;
    bool write_ret = process_write(read_ret);
    // std::cout << "process 节点5" << std::endl;
    if (!write_ret)
    {
        // std::cout << "process 节点6" << std::endl;
        close_conn();
    }
    // std::cout << "process 节点7" << std::endl;
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
