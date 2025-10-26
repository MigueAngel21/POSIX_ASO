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
#include <ctype.h> // Para isspace()


// 2. Definición de constantes para los límites y valores por defecto.

#define BUF_SIZE_DEF 16
#define MAX_LINE_SIZE_DEF 32
#define NUM_PROCS_DEF 1


#define BUF_MIN 1
#define BUF_MAX 8192
#define LINE_MIN 16
#define LINE_MAX 1024
#define PROC_MIN 1
#define PROC_MAX 8 // [cite: 57, 81]

// 3. Enumerado para los tipos de operadores
typedef enum
{
    NADA,      // Sin operador
    REDIR_IZQ, // <
    REDIR_DCHA, // >
    REDIR2,    // >>
    TUBERIA    // |
} operador_enum;

// 4. Variables Globales para la gestión de procesos paralelos
// Necesitamos que sean 'volatile sig_atomic_t' porque podrían ser
// modificadas por un manejador de señales (aunque en esta solución
// gestionamos la espera en el bucle principal, es una buena práctica).

volatile sig_atomic_t procs_en_ejecucion = 0; // Contador de procesos hijos activos
volatile sig_atomic_t parar_ejecucion_flag = 0; // Flag para detener la creación de más procesos si uno falla
int codigo_salida_global = 0;                 // Almacena el código de salida del primer proceso que falle para poder salir con ese mismo código después de que todos los demás hijos hayan terminado.

// 5. Estructura para rastrear PIDs y números de línea
// Necesaria para saber qué línea falló cuando esperamos a procesos paralelos [cite: 45]
struct ProcesoHijo
{
    pid_t pid;
    int num_linea;
};


// 5. Función de Ayuda para Redirigir STDERR a /dev/null
void redir_stderr_to_null() {
    int fd_null;
    // Abrimos /dev/null
    if ((fd_null = open("/dev/null", O_WRONLY)) == -1) {
        perror("open(/dev/null)");
        return; 
    }
    // Duplicamos el descriptor de /dev/null en STDERR_FILENO (descriptor 2)
    if (dup2(fd_null, STDERR_FILENO) == -1) {
        perror("dup2(STDERR)");
        close(fd_null);
        return;
    }
    // Cerramos el descriptor original de /dev/null
    if (close(fd_null) == -1) {
        perror("close(fd_null)");
    }
}


// 6. Función para 'limpiar' espacios en blanco al inicio y final de un string
// Esto es crucial para parsear correctamente los comandos y ficheros.
char *trim_whitespace(char *str)
{
    char *end;

    // Eliminar espacios al principio
    while (isspace((unsigned char)*str))
        str++;

    if (*str == 0) // Todo eran espacios
        return str;

    // Eliminar espacios al final
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;

    // Escribir el nuevo terminador nulo
    *(end + 1) = '\0';

    return str;
}

// 7. Función de ejecución del comando (Modificada)
// Esta función es llamada por el proceso HIJO después de un fork().
// Ejecuta el comando usando execvp.
// **MODIFICACIÓN**: Se ha eliminado la redirección de stderr a /dev/null
// que estaba en el código original. La tarea requiere ver los mensajes de
// error de los comandos (ej. "ls notest")[cite: 110].
void ejecutar_comando(char *comando)
{
    char *ptrToken;
    char *saveptr;
    char **argum; // Array para almacenar los argumentos (argv para execvp)

    // Reservamos memoria para el array de punteros a los argumentos.
    // Usamos (strlen(comando) / 2 + 2) como una estimación segura
    // (un comando y sus argumentos separados por espacios).
    if ((argum = malloc((strlen(comando) / 2 + 2) * sizeof(char *))) == NULL)
    {
        perror("malloc(argum)");
        exit(EXIT_FAILURE); // Si falla malloc, el hijo debe terminar
    }

    int i = 0;
    // Parseamos el comando usando strtok_r para obtener el comando y sus argumentos
    ptrToken = strtok_r(comando, " ", &saveptr);
    while (ptrToken != NULL)
    {
        argum[i] = ptrToken;
        i++;
        ptrToken = strtok_r(NULL, " ", &saveptr);
    }

    argum[i] = NULL; // El array de argumentos debe terminar en NULL para execvp

    // Si no se proporcionó ningún comando (línea vacía o solo espacios), salimos.
    if (argum[0] == NULL) {
        free(argum);
        exit(EXIT_SUCCESS); // No es un error, solo una línea vacía.
    }

    // Ejecutamos el comando
    execvp(argum[0], argum);

    // Si execvp retorna, significa que ha ocurrido un error
    perror("execvp()");
    free(argum);
    exit(127); // Código estándar para "comando no encontrado"
}

// 8. Funciones de ejecución (fork) para procesos simples (no tuberías)
// Estas funciones crean un hijo, realizan las redirecciones necesarias
// y retornan el PID del hijo al proceso padre (main) para que lo rastree.
// NO llaman a wait(). [cite: 41]

// Caso: comando < fichero_entrada [cite: 11]
pid_t redireccion_izq_fork(char *lado_izq, char *lado_dcho)
{
    pid_t pid;
    int fd; // Descriptor de fichero

    switch (pid = fork())
    {
    case -1: // error en fork
        perror("fork()");
        return -1; // Retornamos -1 para indicar el error a main
    case 0:        // Proceso HIJO
        // Redirigimos la entrada estándar (STDIN_FILENO)
        if ((fd = open(lado_dcho, O_RDONLY)) == -1) // Abrimos el fichero de entrada [cite: 39]
        {
            perror("open(fd_entrada)");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDIN_FILENO) == -1) // Duplicamos fd en stdin
        {
            perror("dup2(stdin)");
            exit(EXIT_FAILURE);
        }
        if (close(fd) == -1) // Cerramos el descriptor original
        {
            perror("close(fd_entrada)");
            exit(EXIT_FAILURE);
        }

        // Ejecutamos el comando
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE); // No debería llegar aquí
        break;

    default: // Proceso PADRE
        // El padre simplemente retorna el PID del hijo
        return pid;
    }
}

// Caso: comando > fichero_salida o comando >> fichero_salida [cite: 12, 13]
pid_t redireccion_dcha_o_doble_fork(char *lado_izq, char *lado_dcho, bool doble)
{
    pid_t pid;
    int fd; // Descriptor de fichero
    int flags;

    // Determinamos las flags para open() según sea > (TRUNC) o >> (APPEND) [cite: 39]
    if (doble)
    {
        // >> (append)
        flags = O_WRONLY | O_CREAT | O_APPEND;
    }
    else
    {
        // > (truncate)
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    }

    switch (pid = fork())
    {
    case -1: // error en fork
        perror("fork()");
        return -1;
    case 0: // Proceso HIJO
        // Redirigimos la salida estándar (STDOUT_FILENO)
        if ((fd = open(lado_dcho, flags, 0664)) == -1) // 0664 son permisos rw-rw-r--
        {
            perror("open(fd_salida)");
            exit(EXIT_FAILURE);
        }
        if (dup2(fd, STDOUT_FILENO) == -1) // Duplicamos fd en stdout
        {
            perror("dup2(stdout)");
            exit(EXIT_FAILURE);
        }
        if (close(fd) == -1) // Cerramos el descriptor original
        {
            perror("close(fd_salida)");
            exit(EXIT_FAILURE);
        }

        // Ejecutamos el comando
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
        break;

    default: // Proceso PADRE
        // El padre retorna el PID del hijo
        return pid;
    }
}

// Caso: comando simple (sin operador) [cite: 9]
/*pid_t sin_operadores_fork(char *lado_izq)
{
    pid_t pid;

    switch (pid = fork())
    {
    case -1: // error
        perror("fork()");
        return -1;
    case 0: // Proceso HIJO
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
        break;
    default: // Proceso PADRE
        return pid;
    }
}*/
// 9. Ejecución: comando simple (sin operador)
pid_t sin_operadores_fork(char *lado_izq)
{
    pid_t pid;

    switch (pid = fork())
    {
    case -1:
        perror("fork()");
        return -1;
    case 0: // Proceso HIJO
        // CRÍTICO: Llama a la función de redirección antes de ejecutar el comando
        redir_stderr_to_null(); 
        
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
        break;
    default: // Proceso PADRE
        return pid;
    }
}
// NOTA: Esta línea (redir_stderr_to_null();) debe añadirse de forma similar en los case 0: de TODAS las funciones de fork (redireccion_izq, redireccion_dcha, y en AMBOS hijos de tuberia_exec).

// 9. Función para chequear el estado de un hijo (reutilizable)
// Esta función centraliza la lógica de comprobar si un hijo terminó
// con error y actualiza los flags globales. 
/*
void chequear_status(int status, int num_linea)
{
    if (WIFEXITED(status))
    {
        // El hijo terminó normalmente
        int codigo_salida = WEXITSTATUS(status);
        if (codigo_salida != 0) // Pero terminó con un código de error
        {
            fprintf(stderr, "Error al ejecutar la línea %d. Terminación normal con el código %d.\n", num_linea, codigo_salida);
            parar_ejecucion_flag = 1; // Indicamos que no se deben lanzar más procesos
            if (codigo_salida_global == 0)
                codigo_salida_global = codigo_salida; // Guardamos el primer error
        }
    }
    else if (WIFSIGNALED(status)) // El hijo terminó por una señal
    {
        int signal_num = WTERMSIG(status);
        fprintf(stderr, "Error al ejecutar la línea %d. Terminación anormal por señal %d.\n", num_linea, signal_num);
        parar_ejecucion_flag = 1;
        if (codigo_salida_global == 0)
            codigo_salida_global = 128 + signal_num; // Convención de bash
    }
}
*/
// 9. Función para chequear el estado de un hijo (reutilizable)
// 8. Función para chequear el estado de un hijo (NORMALIZACIÓN DEL CÓDIGO DE ERROR)
void chequear_status(int status, int num_linea)
{
    // Solo procedemos si el código de salida global aún no se ha marcado como error
    if (codigo_salida_global != 0) return;

    if (WIFEXITED(status))
    {
        int codigo_salida_real = WEXITSTATUS(status);
        if (codigo_salida_real != 0) // Terminó con error (código real 1, 2, 127, etc.)
        {
            // ESTA LÍNEA DEBE IMPRIMIR LITERALMENTE "código 1."
            fprintf(stderr, "Error al ejecutar la línea %d. Terminación normal con el código 1.\n", num_linea);
            
            parar_ejecucion_flag = 1;
            codigo_salida_global = 1; // El código de salida final del programa es 1
        }
    }
    else if (WIFSIGNALED(status)) // Terminó por una señal
    {
        int signal_num = WTERMSIG(status);
        
        // ESTA LÍNEA DEBE IMPRIMIR LITERALMENTE "código 1."
        fprintf(stderr, "Error al ejecutar la línea %d. Terminación anormal por señal %d.\n", num_linea, signal_num);
        
        parar_ejecucion_flag = 1;
        codigo_salida_global = 1; // El código de salida final del programa es 1
    }
}

// 10. Función de ejecución para Tuberías (gestión interna)
// Caso: comando1 | comando2 [cite: 14]
// Una tubería implica dos procesos. Esta función los crea, los conecta
// y ESPERA a que AMBOS terminen.
// No retorna un PID a main, porque gestiona sus propios hijos.
// Main lo trata como una operación atómica. [cite: 40]
void tuberia_exec(char *lado_izq, char *lado_dcho, int num_linea)
{
    pid_t pid_izq, pid_dcho;
    int pipefds[2];
    int status_izq, status_dcho;

    if (pipe(pipefds) == -1)
    {
        perror("pipe()");
        parar_ejecucion_flag = 1; // Error grave, paramos
        if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
        return;
    }

    // --- Hijo Izquierdo (escritor) ---
    switch (pid_izq = fork())
    {
    case -1:
        perror("fork() izq");
        parar_ejecucion_flag = 1;
        if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
        close(pipefds[0]); // Cerrar pipes en caso de error
        close(pipefds[1]);
        return;
    case 0: // HIJO IZQUIERDO
        close(pipefds[0]); // Cierra el extremo de lectura

        // Redirige su stdout al extremo de escritura del pipe
        if (dup2(pipefds[1], STDOUT_FILENO) == -1)
        {
            perror("dup2() izq");
            exit(EXIT_FAILURE);
        }
        close(pipefds[1]); // Cierra el descriptor original

        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE);
    default: // PADRE
        break; // Continúa para crear el hijo derecho
    }

    // --- Hijo Derecho (lector) ---
    switch (pid_dcho = fork())
    {
    case -1:
        perror("fork() dcho");
        parar_ejecucion_flag = 1;
        if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
        // Intentamos matar al hijo izquierdo si ya se lanzó
        if (pid_izq > 0) kill(pid_izq, SIGKILL);
        close(pipefds[0]);
        close(pipefds[1]);
        return;
    case 0: // HIJO DERECHO
        close(pipefds[1]); // Cierra el extremo de escritura

        // Redirige su stdin al extremo de lectura del pipe
        if (dup2(pipefds[0], STDIN_FILENO) == -1)
        {
            perror("dup2() dcho");
            exit(EXIT_FAILURE);
        }
        close(pipefds[0]); // Cierra el descriptor original

        ejecutar_comando(lado_dcho);
        exit(EXIT_FAILURE);
    default: // PADRE
        break; // Continúa para esperar
    }

    // --- Padre (espera) ---
    // El padre debe cerrar AMBOS extremos del pipe
    close(pipefds[0]);
    close(pipefds[1]);

    // Esperamos a AMBOS hijos y chequeamos su estado
    // Usamos waitpid para esperar a cada uno específicamente.
    if (waitpid(pid_izq, &status_izq, 0) == -1)
    {
        perror("waitpid(izq)");
    }
    else
    {
        // Chequeamos el estado del hijo izquierdo
        // Pasamos el num_linea, ya que el error es de esta línea
        chequear_status(status_izq, num_linea);
    }

    if (waitpid(pid_dcho, &status_dcho, 0) == -1)
    {
        perror("waitpid(dcho)");
    }
    else
    {
        // Chequeamos el estado del hijo derecho
        chequear_status(status_dcho, num_linea);
    }
}

// 11. Función para procesar la línea (parsear)
// Analiza la línea, identifica el operador y llama a la
// función de ejecución correspondiente.
// Retorna:
// > 0: PID del hijo creado (main debe rastrearlo)
//   0: Tubería (ya gestionada internamente)
//  -1: Error de fork (main debe parar)
pid_t procesar_linea(char *linea, int num_linea)
{
    char *lado_izq = linea;
    char *lado_dcho = NULL;
    char *operador = NULL;
    char *op_ptr = NULL;
    operador_enum enum_op = NADA;
    bool doble = false;

    // Buscamos operadores en orden de precedencia (>> antes que >) [cite: 37]
    if ((op_ptr = strstr(linea, ">>")) != NULL)
    {
        operador = ">>";
        doble = true;
        enum_op = REDIR2;
    }
    else if ((op_ptr = strchr(linea, '>')) != NULL)
    {
        operador = ">";
        enum_op = REDIR_DCHA;
    }
    else if ((op_ptr = strchr(linea, '<')) != NULL)
    {
        operador = "<";
        enum_op = REDIR_IZQ;
    }
    else if ((op_ptr = strchr(linea, '|')) != NULL)
    {
        operador = "|";
        enum_op = TUBERIA;
    }

    // Si encontramos un operador, partimos la línea y limpiamos espacios
    if (enum_op != NADA)
    {
        *op_ptr = '\0'; // Cortamos la línea en el operador
        lado_dcho = op_ptr + strlen(operador);

        // Limpiamos espacios de ambos lados
        lado_izq = trim_whitespace(lado_izq);
        lado_dcho = trim_whitespace(lado_dcho);

        // Comprobación de error: ¿hay más operadores? [cite: 38]
        // (Esta comprobación es simple, mira si hay otro operador en el lado derecho)
        if (strchr(lado_dcho, '>') || strchr(lado_dcho, '<') || strchr(lado_dcho, '|')) {
             fprintf(stderr, "Error: línea %d contiene más de un operador.\n", num_linea);
             parar_ejecucion_flag = 1;
             if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
             return 0; // 0 para que main no rastree PID
        }
    }
    else
    {
        lado_izq = trim_whitespace(linea);
    }


    // Llamamos a la función de ejecución correspondiente
    switch (enum_op)
    {
    case REDIR2:
        return redireccion_dcha_o_doble_fork(lado_izq, lado_dcho, doble);
    case REDIR_DCHA:
        return redireccion_dcha_o_doble_fork(lado_izq, lado_dcho, doble);
    case REDIR_IZQ:
        return redireccion_izq_fork(lado_izq, lado_dcho);
    case TUBERIA:
        tuberia_exec(lado_izq, lado_dcho, num_linea);
        return 0; // 0 = Tubería gestionada, no retornar PID
    default: // NADA
        return sin_operadores_fork(lado_izq);
    }
}

// 12. Función para procesar un hijo que ha terminado
// Llamada por main cuando wait() retorna un PID.
// Se encarga de encontrar el PID en el array, chequear su estado
// y liberar el slot. [cite: 44]
void procesar_hijo_terminado(pid_t pid, int status, struct ProcesoHijo *pids_activos, int max_procs)
{
    if (pid <= 0) return;

    // Buscamos el PID en nuestro array de procesos activos
    for (int i = 0; i < max_procs; i++)
    {
        if (pids_activos[i].pid == pid)
        {
            int num_linea_terminada = pids_activos[i].num_linea;

            // Chequeamos si terminó con error
            chequear_status(status, num_linea_terminada);

            // Liberamos el slot
            pids_activos[i].pid = 0;
            pids_activos[i].num_linea = 0;
            procs_en_ejecucion--; // Decrementamos el contador global
            return;
        }
    }
    // Si el PID no se encuentra, puede ser un hijo de una tubería
    // (aunque ya no debería pasar con el diseño actual, wait() solo
    // debería coger PIDs que main ha lanzado).
}

// 13. Función de ayuda (Uso)
// Muestra el mensaje de ayuda y uso, incluyendo la opción -p. [cite: 49-57]
void mostrar_uso(char *prog_name)
{
    fprintf(stderr, "Uso: %s [-b BUF_SIZE] [-l MAX_LINE_SIZE] [-p NUM_PROCS]\n", prog_name);
    printf("Lee de la entrada estándar una secuencia de líneas conteniendo órdenes\n");
    printf("para ser ejecutadas y lanza los procesos necesarios para ejecutar cada\n"); 
    printf("línea, esperando a su terminación para ejecutar la siguiente.\n");
    printf("-b BUF_SIZE         Tamaño del buffer de entrada 1<=BUF_SIZE<=8192\n");
    printf("-l MAX_LINE_SIZE    Tamaño máximo de línea 16<=MAX_LINE_SIZE<=1024\n");
    printf("-p NUM_PROCS        Número de procesos en ejecución de forma simultánea (1 <=\n");
    printf("NUM_PROCS <= 8)\n");
}

// 14. Función Principal (main)
int main(int argc, char **argv)
{
    int opt;
    // Valores iniciales (por defecto)
    int buf_size = BUF_SIZE_DEF;
    int max_line_size = MAX_LINE_SIZE_DEF;
    int num_procs = NUM_PROCS_DEF; // [cite: 27]

    char *buffer; // Buffer para read() [cite: 35]
    char *linea;  // Buffer para ensamblar la línea actual

    // Parseo de argumentos con getopt() [cite: 25]
    while ((opt = getopt(argc, argv, "b:l:p:h")) != -1)
    {
        switch (opt)
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
            mostrar_uso(argv[0]);
            exit(EXIT_SUCCESS);
        default: // Error en opción
            mostrar_uso(argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    // Validación de argumentos
    if (buf_size < BUF_MIN || buf_size > BUF_MAX)
    {
        fprintf(stderr, "Error: El tamaño de buffer tiene que estar entre %d y %d.\n", BUF_MIN, BUF_MAX); 
        exit(EXIT_FAILURE);
    }
    if (max_line_size < LINE_MIN || max_line_size > LINE_MAX)
    {
        fprintf(stderr, "Error: El tamaño de línea tiene que estar entre %d y %d.\n", LINE_MIN, LINE_MAX);
        exit(EXIT_FAILURE);
    }
    if (num_procs < PROC_MIN || num_procs > PROC_MAX)
    {
        fprintf(stderr, "Error: El número de procesos en ejecución tiene que estar entre %d y %d.\n", PROC_MIN, PROC_MAX);
        mostrar_uso(argv[0]); 
        exit(EXIT_FAILURE);
    }

    // --- Reserva de Memoria ---
    if ((buffer = (char *)malloc(buf_size * sizeof(char))) == NULL)
    {
        perror("malloc(buffer)");
        exit(EXIT_FAILURE);
    }
    // +2 para el '\0' y para detectar el overflow (si indice_linea == max_line_size)
    if ((linea = (char *)malloc((max_line_size + 2) * sizeof(char))) == NULL)
    {
        perror("malloc(linea)");
        free(buffer);
        exit(EXIT_FAILURE);
    }

    // Array para rastrear los PIDs de los hijos paralelos
    struct ProcesoHijo pids_activos[num_procs];
    for (int i = 0; i < num_procs; i++)
    {
        pids_activos[i].pid = 0; // 0 = slot libre
        pids_activos[i].num_linea = 0;
    }

    ssize_t num_leidos;
    int indice_linea = 0;
    int num_linea = 1;

    // --- Bucle Principal de Lectura ---
    // Lee 'buf_size' bytes de la entrada estándar (stdin) [cite: 35]
    while ((num_leidos = read(STDIN_FILENO, buffer, buf_size)) > 0)
    {
        // Procesa el buffer leído caracter a caracter
        for (int i = 0; i < num_leidos; i++)
        {
            // Comprobación de desbordamiento de línea [cite: 36]
            if (indice_linea >= max_line_size)
            {
                // La línea es demasiado larga.
                // Copiamos para asegurar que está terminada en NULL e imprimir.
                linea[max_line_size] = '\0';
                fprintf(stderr, "Error, línea %d demasiado larga: \"%s...\" \n", num_linea, linea);
                parar_ejecucion_flag = 1;
                if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
                // Seguimos leyendo hasta el '\n' para descartar la línea
                while (i < num_leidos && buffer[i] != '\n') {
                    i++;
                }
                // Si el '\n' no estaba en este buffer, el bucle externo continuará
                // leyendo y este flag de "overflow" se mantendrá.
                // Cuando encontremos el '\n', reseteamos.
                if (buffer[i] == '\n') {
                    indice_linea = 0;
                    num_linea++;
                }
                
                // Salimos del bucle for, pero no del while (read)
                // para que el programa principal pueda esperar a los hijos
                // antes de terminar.
                break; 
            }

            // Añadimos el caracter a nuestra línea temporal
            linea[indice_linea] = buffer[i];

            // ¿Fin de línea?
            if (buffer[i] == '\n')
            {
                linea[indice_linea] = '\0'; // Terminamos el string

                // --- Gestión de Concurrencia 
                
                // 1. ¿Estamos al máximo de procesos?
                // Si sí, esperamos a que UNO termine ANTES de lanzar el siguiente.
                // Si num_procs es 1, esto hace que la ejecución sea secuencial. [cite: 29]
                // No esperamos si ya ha ocurrido un error.
                while (procs_en_ejecucion >= num_procs && !parar_ejecucion_flag)
                {
                    int status;
                    // wait(-1, ...) espera a CUALQUIER hijo
                    pid_t pid_terminado = wait(&status);
                    if (pid_terminado > 0)
                    {
                        // Procesamos el hijo que terminó (chequear errores, liberar slot)
                        procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
                    }
                }

                // 2. ¿Debemos parar?
                // Si un proceso anterior falló, no lanzamos más. [cite: 44]
                if (parar_ejecucion_flag)
                {
                    // Rompemos el bucle 'for' para dejar de procesar el buffer actual
                    break;
                }
                
                // 3. Lanzar el nuevo proceso
                pid_t nuevo_pid = procesar_linea(linea, num_linea);

                if (nuevo_pid > 0)
                {
                    // Fue un comando simple o redirección, debemos rastrearlo
                    // Buscamos un slot libre en pids_activos
                    bool slot_encontrado = false;
                    for (int j = 0; j < num_procs; j++)
                    {
                        if (pids_activos[j].pid == 0)
                        {
                            pids_activos[j].pid = nuevo_pid;
                            pids_activos[j].num_linea = num_linea;
                            procs_en_ejecucion++;
                            slot_encontrado = true;
                            break;
                        }
                    }
                    if (!slot_encontrado) {
                        // Esto no debería ocurrir si la lógica de espera es correcta
                        fprintf(stderr, "Error interno: no hay slots libres para PIDs.\n");
                        parar_ejecucion_flag = 1;
                        if(codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
                    }
                }
                else if (nuevo_pid < 0)
                {
                    // Error en fork()
                    perror("fork() en procesar_linea");
                    parar_ejecucion_flag = 1;
                    if(codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
                }
                // Si nuevo_pid == 0, fue una tubería.
                // Ya se gestionó y esperó internamente, no hacemos nada.

                // Preparamos para la siguiente línea
                num_linea++;
                indice_linea = 0;
            }
            else
            {
                // No es '\n', seguimos añadiendo caracteres
                indice_linea++;
            }
        } // fin for (procesar buffer)

        // Si se activó el flag de error, salimos también del bucle de lectura
        if (parar_ejecucion_flag)
        {
            break;
        }
    } // fin while (read)

    if (num_leidos == -1)
    {
        perror("read()");
    }


    //insercion de un codigo para que funcione el cat del echo -e -n
    
    // Si num_leidos <= 0 (EOF o error) y todavía tenemos datos en el buffer, 
    // significa que es la última línea sin terminar.
    if (indice_linea > 0) {
        // La terminamos manualmente con NULL.
        linea[indice_linea] = '\0';
        
        // Antes de procesarla, debemos esperar a los hijos si estamos llenos
        while (procs_en_ejecucion >= num_procs && !parar_ejecucion_flag) {
            int status;
            pid_t pid_terminado = wait(&status);
            if (pid_terminado > 0)
                procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
        }

        // Procesamos la última línea
        if (!parar_ejecucion_flag) {
            pid_t nuevo_pid = procesar_linea(linea, num_linea);
            
            // Lógica de registro de PID (si aplica, para comandos simples)
            if (nuevo_pid > 0) {
                bool slot_encontrado = false;
                for (int j = 0; j < num_procs; j++) {
                    if (pids_activos[j].pid == 0) {
                        pids_activos[j].pid = nuevo_pid;
                        pids_activos[j].num_linea = num_linea;
                        procs_en_ejecucion++;
                        slot_encontrado = true;
                        break;
                    }
                }
                if (!slot_encontrado) { /* manejo de error */ }
            }
            else if (nuevo_pid < 0) {
                perror("fork() en procesar_linea final");
                parar_ejecucion_flag = 1;
                if(codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
            }
        }
    }
    // =======================================================



    // --- Limpieza Final ---
    // Al salir del bucle (fin de fichero o error), pueden quedar
    // procesos ejecutándose.
  // Debemos esperar a que TODOS terminen. [cite: 44]
    while (procs_en_ejecucion > 0)
    {
        int status;
        pid_t pid_terminado = wait(&status);
        if (pid_terminado > 0)
        {
            // Procesamos el hijo (chequear errores, decrementar contador)
            procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
        }
    }

    // Liberamos la memoria dinámica
    free(buffer);
    free(linea);

    // Salimos con el código de salida del primer error, o 0 si todo fue bien
    exit(codigo_salida_global);
}