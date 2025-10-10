#define _POSIX_C_SOURCE 200809L

#include <stdio.h> // E/S estándar (printf, fprintf, perror...)
#include <string.h> // funciones de manejo de cadenas (strerror, strlen...)
#include <stdarg.h> // para funciones con argumentos variádicos (vfprintf)
#include <stdbool.h> // tipo bool en C
#include <getopt.h> // getopt para parseo de opciones
#include <stdlib.h> // utilidades generales (malloc, free, exit...)
#include <errno.h> // definiciones de errno y macros relacionadas
#include <unistd.h> // read, write, fork, exec, pipe, dup2
#include <sys/types.h> // tipos POSIX (pid_t, ssize_t...)
#include <sys/wait.h> // wait, waitpid, macros para estado de hijo
#include <fcntl.h> // constantes y flags para open()
#include <signal.h> // manejo de señales (sigaction, SIGCHLD)
#include <sys/stat.h> // para modos de archivo (S_IRWXU...)

#define BUF_SIZE_DEFAULT 16
#define MAX_LINE_SIZE_DEFAULT 32
#define MAX_PROCS_DEFAULT 1
// -b
#define BUF_SIZE_MIN 1
#define BUF_SIZE_MAX 8192
// -l
#define MAX_LINE_SIZE_MIN 16
#define MAX_LINE_SIZE_MAX 1024
// -p
#define MAX_PROCS_MIN 1
#define MAX_PROCS_MAX 8


void print_help(char* program_name)
{
    fprintf(stderr, "Uso: %s [-b BUF_SIZE] [-l MAX_LINE_SIZE] [-p NUM_PROCS]\n", program_name);
    printf("Lee de la entrada estándar una secuencia de líneas conteniendo órdenes\n");
    printf("para ser ejecutadas y lanza los procesos necesarios para ejecutar cada\n"); 
    printf("línea, esperando a su terminación para ejecutar la siguiente.\n");
    printf("-b BUF_SIZE         Tamaño del buffer de entrada 1<=BUF_SIZE<=8192\n");
    printf("-l MAX_LINE_SIZE    Tamaño máximo de línea 16<=MAX_LINE_SIZE<=1024\n");
    printf("-p NUM_PROCS        Número de procesos en ejecución de forma simultánea (1 <=\n");
    printf("NUM_PROCS <= 8)\n");
}

int main(int argc, char *argv[])
{
    int opt;
    int buf_size = BUF_SIZE_DEFAULT;
    int max_line_size = MAX_LINE_SIZE_DEFAULT;
    int num_procs = MAX_PROCS_DEFAULT;

    while((opt = getopt(argc, argv, "b:l:p:h")) != -1)
    {
        switch(opt)
        {
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
                print_help(argv[0]);
                exit(EXIT_SUCCESS);
            case ':':
                fprintf(stderr, "%s: la opción '-%c' requiere un argumento.\n", argv[0], optopt);
                exit(EXIT_FAILURE);
            case '?': //Esto hay que preguntar si podemos
            default:
                print_help(argv[0]);
                exit(EXIT_FAILURE);
        }
    }


    if(buf_size < BUF_SIZE_MIN || buf_size > BUF_SIZE_MAX)
    {
        fprintf(stderr, "Error: el tamaño del buffer debe estar entre %d y %d.\n", BUF_SIZE_MIN, BUF_SIZE_MAX);
        exit(EXIT_FAILURE);
    }

    //TODO esto.
   
    //FIN TODO
    
    if(num_procs < MAX_PROCS_MIN || num_procs > MAX_PROCS_MAX)
    { 
        fprintf(stderr, "Error: el número de procesos en ejecución debe estar entre %d y %d.\n", MAX_PROCS_MIN, MAX_PROCS_MAX);
        print_help(argv[0]);
        exit(EXIT_FAILURE);
    }

    /* Preguntar al profesor
    optind = 1;
    if (optind < argc){
        fprintf(stderr, "Error: No se admiten parametros adicionales\n");
        fprintf(stderr, "Uso: %s [-t] [-x NUMERO] [-y NUMERO] [-s STRING]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    */
    
    ssize_t num_read;
    char *buf;
    
    if((buf = (char *) malloc(buf_size * sizeof(char))) == NULL)
    {
        perror("malloc()");
        exit(EXIT_FAILURE);
    }   
    // Lectura de líneas de la entrada estándar
    num_read = read(STDIN_FILENO, buf, buf_size);
    fprintf(stderr,"Mi buffer es: %s y mi num_read es: %ld\n",buf,num_read);
    if( num_read > MAX_LINE_SIZE_MAX)
    {
        fprintf(stderr, "Error: línea %ld demasiado larga: %s.\n", (long)num_read, buf);
        exit(EXIT_FAILURE);
    }
    else if(num_read < MAX_LINE_SIZE_MIN)
    {
        fprintf(stderr, "Error: línea %ld demasiado corta: %s.\n", (long)num_read, buf);
        exit(EXIT_FAILURE);
    }
    // Procesamiento de cada línea leída
    // Lanzamiento de procesos para ejecutar las órdenes
    // Control del número de procesos en ejecución simultánea
    // Espera de la terminación de los procesos lanzados
    // Liberación de recursos y salida del programa
    
    printf("BUF_SIZE = %d\n", buf_size);
    printf("MAX_LINE_SIZE = %d\n", max_line_size);
    printf("NUM_PROCS = %d\n", num_procs);

    
    free(buf);
    return EXIT_SUCCESS;
}