#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

//valores por defecto
#define BUF_SIZE_DEF 16
#define MAX_LINE_SIZE_DEF 32
#define NUM_PROCS_DEF 1

#define BUF_MIN 1
#define BUF_MAX 8192

#define LINE_MIN 16
#define LINE_MAX 1024

#define PROC_MIN 1
#define PROC_MAX 8

//enumerado para los operadores de una linea
typedef enum {
    NADA,
    REDIR_IZQ,
    REDIR_DCHA,
    REDIR2,
    TUBERIA
} operador_enum;


int procs_en_ejecucion = 0;
int parar_ejecucion_flag = 0; //flag para detener la ejecucion si un proceso falla
int codigo_salida_global = EXIT_SUCCESS; 

//estructura para los hijos activos
struct ProcesoHijo {
    pid_t pid;
    int num_linea;
};

//funcion para eliminar los espacios en blanco
char *trim_whitespace(char *str) {
    char *end;

    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == '\0') {
        return str;
    }
    
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    
    end[1] = '\0';
    return str;
}

//funcion que ejecuta el comando dado por argumento
void ejecutar_comando(char *comando) {
    char *ptrToken;
    char *saveptr; //puntero para strtok_r
    char **argum; //array de argumentos para execvp
    int i = 0;

    if ((argum = malloc((strlen(comando) / 2 + 2) * sizeof(char *))) == NULL) {
        perror("malloc(argum)");
        exit(EXIT_FAILURE);
    }

    ptrToken = strtok_r(comando, " ", &saveptr);
    while (ptrToken != NULL) {
        argum[i] = ptrToken;
        i++;
        ptrToken = strtok_r(NULL, " ", &saveptr);
    }

    argum[i] = NULL;

    if (argum[0] == NULL) {
        free(argum);
        exit(EXIT_SUCCESS);
    }

    // redirigir stderr a /dev/null para no mostrar errores por la terminal
    int fd_null = open("/dev/null", O_WRONLY);
    if(fd_null != -1) {
        dup2(fd_null, STDERR_FILENO);
        close(fd_null);
    }
    
    execvp(argum[0], argum);
    perror("execvp()");
    free(argum);
    exit(127);
}

//funcion que crea un hijo para ejecutar un comando con el operador <
pid_t redireccion_izq_fork(char *lado_izq, char *lado_dcho) {
    pid_t pid;
    int fd;

    pid = fork();
    if (pid == -1) {
        perror("fork()");
        return -1;
    }

    if (pid == 0) {
        if ((fd = open(lado_dcho, O_RDONLY)) == -1) {
            perror("open(fd_entrada)");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2(stdin)");
            exit(EXIT_FAILURE);
        }
        close(fd);
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
    }

    return pid;
}

//funcion que crea un hijo para ejecutar un comando con el operador >> o >
pid_t redireccion_dcha_o_doble_fork(char *lado_izq, char *lado_dcho, bool doble) {
    pid_t pid;
    int fd;
    int flags;

    if (doble) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    }

    pid = fork();
    if (pid == -1) {
        perror("fork()");
        return -1;
    }

    if (pid == 0) {
        if ((fd = open(lado_dcho, flags, 0664)) == -1) {
            perror("open(fd_salida)");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2(stdout)");
            exit(EXIT_FAILURE);
        }
        close(fd);
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
    }

    return pid;
}

//funcion que crea un hijo para ejecutar un comando que no tiene operadores
pid_t sin_operadores_fork(char *lado_izq) {
    pid_t pid;

    pid = fork();
    if (pid == -1) {
        perror("fork()");
        return -1;
    }

    if (pid == 0) {
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
    }

    return pid;
}

//analiza el status de un proceso hijo que ha terminado
void chequear_status(int status, int num_linea) {
    if (codigo_salida_global != 0) return;

    if (WIFEXITED(status)) {
        int codigo_salida_real = WEXITSTATUS(status);
        if (codigo_salida_real != 0) {
            fprintf(stderr, "Error al ejecutar la línea %d. Terminación normal con código %d.\n", 
                    num_linea, codigo_salida_real);
            parar_ejecucion_flag = 1;
            codigo_salida_global = EXIT_FAILURE;
        }
    } else if (WIFSIGNALED(status)) {
        int signal_num = WTERMSIG(status);
        fprintf(stderr, "Error al ejecutar la línea %d. Terminación anormal por señal %d.\n", 
                num_linea, signal_num);
        parar_ejecucion_flag = 1;
        codigo_salida_global = EXIT_FAILURE;
    }
}

//funcion que crea dos hijos para ejecutar un comando con el operador |
pid_t tuberia_exec(char *lado_izq, char *lado_dcho, int num_linea) {
    int pipefds[2];
    pid_t pid_izq, pid_dcho;

    if (pipe(pipefds) == -1) {
        perror("pipe()");
        return -1;
    }

    // se crea el hijo izquierdo
    pid_izq = fork();
    if (pid_izq == -1) {
        perror("fork() izq");
        close(pipefds[0]);
        close(pipefds[1]);
        return -1;
    }

    if (pid_izq == 0) {
        //hijo izquierdo
        close(pipefds[0]);  //se cierra el extremo de lectura
        
        // se redirege la salida estandar desde la tuberia
        if (dup2(pipefds[1], STDOUT_FILENO) == -1) {
            perror("dup2() izq");
            exit(EXIT_FAILURE);
        }
        close(pipefds[1]);  //se cierra el original
        
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
    }

    // se crea el hijo derecho
    pid_dcho = fork();
    if (pid_dcho == -1) {
        perror("fork() dcho");
        close(pipefds[0]);
        close(pipefds[1]);
        kill(pid_izq, SIGKILL); //matar al izquierdo si falla el derecho
        return -1;
    }

    if (pid_dcho == 0) {
        //hijo derecho
        close(pipefds[1]);  //se cierra el extremo de escritura
        
        //se redirege la salida estandar desde la tuberia
        if (dup2(pipefds[0], STDIN_FILENO) == -1) {
            perror("dup2() dcho");
            exit(EXIT_FAILURE);
        }
        close(pipefds[0]);  //se cierra el original
        
        ejecutar_comando(lado_dcho);
        exit(EXIT_FAILURE);
    }

    //el padre cierra ambos extremos de la tuberia
    close(pipefds[0]);
    close(pipefds[1]);

    //se espera a que los procesos terminen
    int status_izq, status_dcho;
    waitpid(pid_izq, &status_izq, 0);
    waitpid(pid_dcho, &status_dcho, 0);

    //ver si ha habido errores
    if (WIFEXITED(status_izq) && WEXITSTATUS(status_izq) != 0) {
        fprintf(stderr, "Error en comando izquierdo de la línea %d\n", num_linea);
        return -1;
    }
    if (WIFEXITED(status_dcho) && WEXITSTATUS(status_dcho) != 0) {
        fprintf(stderr, "Error en comando derecho de la línea %d\n", num_linea);
        return -1;
    }
    if (WIFSIGNALED(status_izq)) {
        fprintf(stderr, "Comando izquierdo de la línea %d terminado por señal %d\n", 
                num_linea, WTERMSIG(status_izq));
        return -1;
    }
    if (WIFSIGNALED(status_dcho)) {
        fprintf(stderr, "Comando derecho de la línea %d terminado por señal %d\n", 
                num_linea, WTERMSIG(status_dcho));
        return -1;
    }

    //se devuelve 0 para esperar aquí y que el proceso principal no espere de nuevo
    return 0;
}

//funcion para procesar una linea leida (parsea y ejecuta)
pid_t procesar_linea(char *linea, int num_linea) {
    char *lado_izq = linea;
    char *lado_dcho = NULL;
    char *operador = NULL;
    char *op_ptr = NULL;
    operador_enum enum_op = NADA;
    bool doble = false;

    if ((op_ptr = strstr(linea, ">>")) != NULL) {
        operador = ">>";
        doble = true;
        enum_op = REDIR2;
    } else if ((op_ptr = strchr(linea, '>')) != NULL) {
        operador = ">";
        enum_op = REDIR_DCHA;
    } else if ((op_ptr = strchr(linea, '<')) != NULL) {
        operador = "<";
        enum_op = REDIR_IZQ;
    } else if ((op_ptr = strchr(linea, '|')) != NULL) {
        operador = "|";
        enum_op = TUBERIA;
    }

    if (enum_op != NADA) {
        *op_ptr = '\0';
        lado_dcho = op_ptr + strlen(operador);

        lado_izq = trim_whitespace(lado_izq);
        lado_dcho = trim_whitespace(lado_dcho);

        //ver que no haya mas de un operador en la linea
        if (strchr(lado_dcho, '>') || strchr(lado_dcho, '<') || strchr(lado_dcho, '|')) {
            fprintf(stderr, "Error: línea %d contiene más de un operador.\n", num_linea);
            parar_ejecucion_flag = 1;
            codigo_salida_global = EXIT_FAILURE;
            return 0;
        }
    } else {
        lado_izq = trim_whitespace(linea);
    }

    switch (enum_op) {
        case REDIR2:
            return redireccion_dcha_o_doble_fork(lado_izq, lado_dcho, doble);
        case REDIR_DCHA:
            return redireccion_dcha_o_doble_fork(lado_izq, lado_dcho, doble);
        case REDIR_IZQ:
            return redireccion_izq_fork(lado_izq, lado_dcho);
        case TUBERIA:
            tuberia_exec(lado_izq, lado_dcho, num_linea);
            return 0; 
        default:
            return sin_operadores_fork(lado_izq);
    }
}

//funcion para procesar un hijo que ha terminado
void procesar_hijo_terminado(pid_t pid, int status, struct ProcesoHijo *pids_activos, int max_procs) {
    if (pid <= 0) return;

    for (int i = 0; i < max_procs; i++) {
        if (pids_activos[i].pid == pid) {
            chequear_status(status, pids_activos[i].num_linea);
            pids_activos[i].pid = 0;
            pids_activos[i].num_linea = 0;
            procs_en_ejecucion--;
            return;
        }
    }
}

//muestra la ayuda del programa
void mostrar_uso(char *prog_name) {
    fprintf(stderr, "Uso: %s [-b BUF_SIZE] [-l MAX_LINE_SIZE] [-p NUM_PROCS]\n", prog_name);
    printf("Lee de la entrada estándar una secuencia de líneas conteniendo órdenes\n");
    printf("para ser ejecutadas y lanza los procesos necesarios para ejecutar cada\n"); 
    printf("línea, esperando a su terminación para ejecutar la siguiente.\n");
    printf("-b BUF_SIZE         Tamaño del buffer de entrada 1<=BUF_SIZE<=8192\n");
    printf("-l MAX_LINE_SIZE    Tamaño máximo de línea 16<=MAX_LINE_SIZE<=1024\n");
    printf("-p NUM_PROCS        Número de procesos en ejecución de forma simultánea (1 <=\n");
    printf("NUM_PROCS <= 8)\n");
}

int main(int argc, char **argv) {
    int opt;
    int buf_size = BUF_SIZE_DEF;
    int max_line_size = MAX_LINE_SIZE_DEF;
    int num_procs = NUM_PROCS_DEF;

    char *buffer; //buffer de lectura (read)
    char *linea; //buffer de la linea actual

    while ((opt = getopt(argc, argv, "b:l:p:h")) != -1) {
        switch (opt) {
            case 'b':
                buf_size = atoi(optarg);
                break;
            case 'l':
                max_line_size = atoi(optarg);
                break;
            case 'p': 
                num_procs = atoi(optarg);
                break;
            case 'h':
                mostrar_uso(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                mostrar_uso(argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // tratamiento de errores de los argumentos
    if (buf_size < BUF_MIN || buf_size > BUF_MAX) {
        fprintf(stderr, "Error: El tamaño de buffer tiene que estar entre %d y %d.\n", BUF_MIN, BUF_MAX); 
        exit(EXIT_FAILURE);
    }
    if (max_line_size < LINE_MIN || max_line_size > LINE_MAX) {
        fprintf(stderr, "Error: El tamaño de línea tiene que estar entre %d y %d.\n", LINE_MIN, LINE_MAX);
        exit(EXIT_FAILURE);
    }
    if (num_procs < PROC_MIN || num_procs > PROC_MAX) {
        fprintf(stderr, "Error: El número de procesos en ejecución tiene que estar entre %d y %d.\n", 
                PROC_MIN, PROC_MAX);
        exit(EXIT_FAILURE);
    }

    // reservar memoria para los buffers
    if ((buffer = malloc(buf_size * sizeof(char))) == NULL) {
        perror("malloc(buffer)");
        exit(EXIT_FAILURE);
    }

    if ((linea = malloc((max_line_size + 2) * sizeof(char))) == NULL) {
        perror("malloc(linea)");
        free(buffer);
        exit(EXIT_FAILURE);
    }

    // inicializar array de procesos activos
    struct ProcesoHijo pids_activos[num_procs];
    for (int i = 0; i < num_procs; i++) {
        pids_activos[i].pid = 0;
        pids_activos[i].num_linea = 0;
    }

    ssize_t num_leidos; // número de bytes leídos
    int indice_linea = 0; // índice actual en la línea
    int num_linea = 1; // número de línea

    // Bucle principal de lectura
    while ((num_leidos = read(STDIN_FILENO, buffer, buf_size)) > 0) {

        for (int i = 0; i < num_leidos; i++) {
            //comprobar si la línea es demasiado larga
            if (indice_linea >= max_line_size) {
                linea[max_line_size] = '\0';
                fprintf(stderr, "Error, línea %d demasiado larga: \"%s...\" \n", num_linea, linea);
                parar_ejecucion_flag = 1;
                codigo_salida_global = EXIT_FAILURE;
                
                // Saltar al final de la línea actual
                while (i < num_leidos && buffer[i] != '\n') {
                    i++;
                }
                
                if (buffer[i] == '\n') {
                    indice_linea = 0;
                    num_linea++;
                }
                break; 
            }

            linea[indice_linea] = buffer[i];
            //comprobar si se ha llegado al final de la línea
            if (buffer[i] == '\n') {
                linea[indice_linea] = '\0';

                // esperar si hay demasiados procesos en ejecución
                while (procs_en_ejecucion >= num_procs && !parar_ejecucion_flag) {
                    int status;
                    pid_t pid_terminado = wait(&status);
                    if (pid_terminado > 0) {
                        procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
                    }
                }

                //si se activo el flag de error
                if (parar_ejecucion_flag) {
                    break;
                }
                
                //se procesa la linea
                pid_t nuevo_pid = procesar_linea(linea, num_linea);

                if (nuevo_pid > 0) {
                    //slot libre para el nuevo proceso
                    int slot_encontrado = 0;
                    for (int j = 0; j < num_procs; j++) {
                        if (pids_activos[j].pid == 0) {
                            pids_activos[j].pid = nuevo_pid;
                            pids_activos[j].num_linea = num_linea;
                            procs_en_ejecucion++;
                            slot_encontrado = 1;
                            break;
                        }
                    }
                    if (!slot_encontrado) {
                        fprintf(stderr, "Error interno: no hay slots libres para PIDs.\n");
                        parar_ejecucion_flag = 1;
                        codigo_salida_global = EXIT_FAILURE;
                    }
                } else if (nuevo_pid < 0) {
                    perror("fork() en procesar_linea");
                    parar_ejecucion_flag = 1;
                    codigo_salida_global = EXIT_FAILURE;
                }
                //se prepara la siguiente línea
                num_linea++;
                indice_linea = 0;
            } else {
                indice_linea++;
            }
        } 

        if (parar_ejecucion_flag) {
            break;
        }
    } 

    if (num_leidos == -1) {
        perror("read()");
    }
    
    // si se ha terminado sin un \n y sigue habiendo algo, procesar lo que quede
    if (indice_linea > 0 && !parar_ejecucion_flag) {
        linea[indice_linea] = '\0';
        
        while (procs_en_ejecucion >= num_procs && !parar_ejecucion_flag) {
            int status;
            pid_t pid_terminado = wait(&status);
            if (pid_terminado > 0) {
                procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
            }
        }
        // si no hay error, procesar lo que quede
        if (!parar_ejecucion_flag) {
            pid_t nuevo_pid = procesar_linea(linea, num_linea);
            
            if (nuevo_pid > 0) {
                int slot_encontrado = 0;
                for (int j = 0; j < num_procs; j++) {
                    if (pids_activos[j].pid == 0) {
                        pids_activos[j].pid = nuevo_pid;
                        pids_activos[j].num_linea = num_linea;
                        procs_en_ejecucion++;
                        slot_encontrado = 1;
                        break;
                    }
                }
                if (!slot_encontrado) {
                    fprintf(stderr, "Error interno: no hay slots libres para PIDs.\n");
                }
            }
        }
    }

    // esperar a que terminen todos los procesos que quedan
    while (procs_en_ejecucion > 0) {
        int status;
        pid_t pid_terminado = wait(&status);
        if (pid_terminado > 0) {
            procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
        }
    }

    free(buffer);
    free(linea);
    exit(codigo_salida_global);
}