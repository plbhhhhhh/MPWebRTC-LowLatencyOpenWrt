/* -*-mode:c++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <fstream>
#include <iostream>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/wait.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "packetshell.hh"
#include "json.hh"

using json = nlohmann::json;
using namespace std;

int main( int argc, char *argv[] )
{
    try {
        /* clear environment while running as root */
        char ** const user_environment = environ;
        environ = nullptr;

        check_requirements( argc, argv );

        if ( argc < 2 ) {
            throw Exception( "Usage", string( argv[ 0 ] ) + " config_file [program]" );
        }

        std::vector<uint64_t> delays;
        std::vector<std::string> uplinks;
        std::vector<std::string> downlinks;
        std::vector<json> queue_params;
        std::string log_file = "";
        get_config(argv[1], delays, uplinks, downlinks, queue_params, log_file);
        int if_num = delays.size();

        vector< string > program_to_run;
        bool run_interactive_shell = (argc == 2); // 只提供配置文件时运行交互式Shell

        if (argc >= 3) {
            for ( int num_args = 2; num_args < argc; num_args++ ) {
                program_to_run.emplace_back( string( argv[ num_args ] ) );
            }
        }

        PacketShell mp_shell_app( if_num );

        // 创建管道用于通信
        int pipefd[2];
        if (pipe(pipefd) == -1) {
            perror("pipe");
            return EXIT_FAILURE;
        }
        
        mp_shell_app.start_uplink( "[ mpshell ] ",
                                   user_environment,
                                   delays,
                                   uplinks,
                                   queue_params,
                                   log_file,
                                   program_to_run,
                                   pipefd);

        mp_shell_app.start_downlink( delays, downlinks, queue_params, log_file);
        // 如果指定了程序，等待程序执行
        if (!run_interactive_shell) {
            return mp_shell_app.wait_for_exit();
        }

        // 以下是交互式Shell处理
        // 关闭父进程端的写端
        close(pipefd[1]);

        // 等待子进程准备就绪信号
        char buf;
        read(pipefd[0], &buf, 1);
        close(pipefd[0]);

        // 启动交互式Shell
        // const char *shell = getenv("SHELL");
        // if (!shell) shell = "/bin/bash";

        std::cout << "\n[ mpshell ] Network simulation environment is ready.\n";
        std::cout << "[ mpshell ] Entering interactive shell in the simulated network.\n";
        std::cout << "[ mpshell ] Type 'exit' or press Ctrl-D to quit.\n";

        // // 启动shell
        // pid_t pid = fork();
        // if (pid == 0) {
        //     // 创建新的会话和进程组
        //     pid_t sid = setsid();
        //     if (sid < 0) {
        //         perror("setsid");
        //         return EXIT_FAILURE;
        //     }
            
        //     // 设置终端属性
        //     struct termios t;
        //     tcgetattr(STDIN_FILENO, &t);
        //     t.c_lflag |= ECHO | ICANON;
        //     tcsetattr(STDIN_FILENO, TCSANOW, &t);
            
        //     // 启动交互式shell
        //     const char *args[] = {
        //         shell,
        //         "-i",  // 强制交互模式
        //         nullptr
        //     };
            
        //     execvp(args[0], const_cast<char* const*>(args));
        //     perror("execvp");
        //     exit(EXIT_FAILURE);
        // } else if (pid > 0) {
        //     // 父进程: 等待shell退出
        //     int status;
        //     waitpid(pid, &status, 0);
            
        //     std::cout << "[ mpshell ] Shell exited. Cleaning up...\n";
        // } else {
        //     perror("fork");
        //     return EXIT_FAILURE;
        // }

        // 退出前清理环境
        return mp_shell_app.wait_for_exit();
    } catch ( const Exception & e ) {
        e.perror();
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
