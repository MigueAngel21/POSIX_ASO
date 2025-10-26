// 1. Directiva de preprocesador
#define _POSIX_C_SOURCE 200809L

// 2. Inclusión de cabeceras estándar
#include <stdio.h>      // Para funciones de entrada/salida (printf, fprintf, stderr, perror)
#include <stdlib.h>     // Para funciones generales (malloc, free, exit, atoi, EXIT_FAILURE)
#include <unistd.h>     // El núcleo de S.O. (read, write, fork, execvp, dup2, pipe, getopt, STDIN_FILENO, etc.)
#include <string.h>     // Para manipulación de strings (strlen, strstr, strchr, strtok_r)
#include <fcntl.h>      // Para control de ficheros (open, O_RDONLY, O_WRONLY, O_CREAT, etc.)
#include <sys/stat.h>   // Para información de ficheros (usado por fcntl.h para los modos de 'open')
#include <sys/types.h>  // Para tipos de datos primitivos (pid_t, ssize_t)
#include <sys/wait.h>   // Para esperar a procesos (wait, waitpid, WIFEXITED, WEXITSTATUS, etc.)
#include <stdbool.h>    // Para usar el tipo de dato 'bool' (true, false)
#include <errno.h>      // Para la variable 'errno' (aunque 'perror' la usa implícitamente)
#include <ctype.h>      // Para funciones de caracteres (isspace)


// 2. Definición de constantes para los límites y valores por defecto.

// Valores por defecto para los argumentos
[cite_start]#define BUF_SIZE_DEF 16         // Tamaño de buffer por defecto si no se especifica -b [cite: 26]
[cite_start]#define MAX_LINE_SIZE_DEF 32    // Tamaño de línea máximo por defecto si no se especifica -l [cite: 26]
[cite_start]#define NUM_PROCS_DEF 1         // Número de procesos por defecto si no se especifica -p [cite: 27]

// Límites para la validación de argumentos
[cite_start]#define BUF_MIN 1               // Tamaño de buffer mínimo permitido [cite: 56]
[cite_start]#define BUF_MAX 8192            // Tamaño de buffer máximo permitido [cite: 56]
[cite_start]#define LINE_MIN 16             // Tamaño de línea mínimo permitido [cite: 56]
[cite_start]#define LINE_MAX 1024           // Tamaño de línea máximo permitido [cite: 56]
[cite_start]#define PROC_MIN 1              // Número de procesos mínimo permitido [cite: 57]
[cite_start]#define PROC_MAX 8              // Número de procesos máximo permitido [cite: 57, 81]

// 3. Enumerado para los tipos de operadores
// Un tipo 'enum' nos permite dar nombres legibles a los tipos de operaciones
typedef enum
{
    NADA,      // Sin operador (ej. 'ls -l')
    [cite_start]REDIR_IZQ, // < (redirección de entrada) [cite: 11]
    [cite_start]REDIR_DCHA, // > (redirección de salida, truncando) [cite: 12]
    [cite_start]REDIR2,    // >> (redirección de salida, añadiendo) [cite: 13]
    TUBERIA    // | (tubería) [cite_start][cite: 14]
} operador_enum;

// 4. Variables Globales para la gestión de procesos paralelos
// Necesarias para coordinar el proceso padre y sus hijos.

// 'volatile sig_atomic_t' es un tipo especial para variables globales que
// podrían ser modificadas por un manejador de señales (aunque aquí no usemos
// manejadores, es buena práctica para variables que cambian "asincrónicamente"
// como con 'wait').

// Contador de cuántos procesos hijos están actualmente en ejecución.
volatile sig_atomic_t procs_en_ejecucion = 0;
[cite_start]// Bandera que se activa (pone a 1) si un hijo termina con error. [cite: 44]
volatile sig_atomic_t parar_ejecucion_flag = 0;
// Almacena el código de salida del *primer* proceso que falle.
int codigo_salida_global = 0;

// 5. Estructura para rastrear PIDs y números de línea
[cite_start]// Necesaria para saber qué línea falló cuando esperamos a procesos paralelos [cite: 45]
struct ProcesoHijo
{
    pid_t pid;     // El ID del proceso hijo
    int num_linea; // El número de línea que este PID está ejecutando
};


// 5. Función de Ayuda para Redirigir STDERR a /dev/null
// Esta función se llamará desde el proceso HIJO.
void redir_stderr_to_null() {
    int fd_null; // Variable para el descriptor de fichero de /dev/null
    
    // Abrimos el fichero especial /dev/null en modo solo escritura (O_WRONLY)
    if ((fd_null = open("/dev/null", O_WRONLY)) == -1) {
        perror("open(/dev/null)"); // Si falla, informamos (aunque irá a stderr...)
        return; // ...y salimos de la función.
    }
    // Esta es la línea clave: duplicamos el descriptor de /dev/null (fd_null)
    // sobre el descriptor de la salida de error estándar (STDERR_FILENO, que es 2).
    // Ahora, cualquier cosa que se intente escribir en el descriptor 2 irá a /dev/null.
    if (dup2(fd_null, STDERR_FILENO) == -1) {
        perror("dup2(STDERR)"); // Si falla la duplicación
        close(fd_null);       // Cerramos el descriptor que abrimos
        return;               // y salimos.
    }
    // Cerramos el descriptor original de /dev/null.
    // Ya no lo necesitamos porque el descriptor 2 "apunta" al mismo sitio.
    if (close(fd_null) == -1) {
        perror("close(fd_null)"); // Informamos si falla el cierre.
    }
}


// 6. Función para 'limpiar' espacios en blanco al inicio y final de un string
// Recibe un puntero a un string (un array de char).
char *trim_whitespace(char *str)
{
    char *end; // Puntero que apuntará al final del string

    // Bucle: mientras el carácter al que apunta 'str' sea un espacio...
    while (isspace((unsigned char)*str))
        str++; // ...avanzamos el puntero 'str' al siguiente carácter.

    if (*str == 0) // Si el carácter es 0 ('\0'), el string estaba vacío o solo tenía espacios
        return str; // Devolvemos el puntero (que ahora apunta al '\0')

    // Ahora limpiamos el final. 'end' apunta al *último* carácter (antes del '\0').
    end = str + strlen(str) - 1;
    // Bucle: mientras 'end' esté después de 'str' Y el carácter al que apunta 'end' sea un espacio...
    while (end > str && isspace((unsigned char)*end))
        end--; // ...movemos el puntero 'end' hacia atrás.

    // 'end' apunta ahora al último carácter que NO es un espacio.
    // Ponemos el carácter nulo ('\0') justo *después* de él,
    // "cortando" así los espacios del final.
    *(end + 1) = '\0';

    return str; // Devolvemos el puntero al inicio del string "limpio".
}

// 7. Función de ejecución del comando (Modificada)
// Esta función es llamada SIEMPRE por el proceso HIJO.
// Recibe el comando "limpio" (ej. "ls -la")
void ejecutar_comando(char *comando)
{
    char *ptrToken; // Puntero para cada "trozo" (token) del comando
    char *saveptr;  // Puntero interno que usa strtok_r para guardar su posición
    char **argum;   // Puntero a puntero: será nuestro array de argumentos (el 'argv' para execvp)

    // Reservamos memoria para el array de punteros.
    // (strlen(comando) / 2 + 2) es una estimación: "cmd arg1 arg2" (5 chars) -> 3 punteros.
    if ((argum = malloc((strlen(comando) / 2 + 2) * sizeof(char *))) == NULL)
    {
        perror("malloc(argum)"); // Si falla el malloc
        exit(EXIT_FAILURE);     // El hijo termina con error
    }

    int i = 0; // Índice para el array 'argum'
    // Partimos el comando usando el espacio " " como delimitador.
    // strtok_r es la versión segura (reentrante) de strtok.
    ptrToken = strtok_r(comando, " ", &saveptr);
    // Bucle: mientras strtok_r encuentre tokens...
    while (ptrToken != NULL)
    {
        argum[i] = ptrToken; // Guardamos el puntero al token en nuestro array
        i++;                 // Incrementamos el índice
        // Llamamos a strtok_r con NULL para que continúe desde donde se quedó
        ptrToken = strtok_r(NULL, " ", &saveptr);
    }

    argum[i] = NULL; // ¡CRÍTICO! El array de argumentos para execvp debe terminar en NULL.

    // Si argum[0] es NULL, la línea estaba vacía o solo tenía espacios.
    if (argum[0] == NULL) {
        free(argum);        // Liberamos la memoria del array
        exit(EXIT_SUCCESS); // Salimos con éxito (no es un error).
    }

    // ¡La llamada clave! Reemplaza este proceso hijo por el comando solicitado.
    // argum[0] es el comando (ej. "ls")
    // argum es el array completo (ej. {"ls", "-la", NULL})
    execvp(argum[0], argum);

    // Si execvp RETORNA, es que ha habido un ERROR (ej. comando no encontrado).
    perror("execvp()"); // Imprime el error (ej. "execvp(): No such file or directory")
    free(argum);        // Liberamos la memoria
    exit(127);          // Salimos con 127 (código estándar para "comando no encontrado")
}

// 8. Funciones de ejecución (fork) para procesos simples (no tuberías)
// Estas funciones crean un hijo (fork), configuran sus descriptores (dup2)
// y llaman a ejecutar_comando (execvp). El padre solo retorna el PID.

[cite_start]// Caso: comando < fichero_entrada [cite: 11]
pid_t redireccion_izq_fork(char *lado_izq, char *lado_dcho)
{
    pid_t pid; // Variable para almacenar el PID
    int fd;    // Variable para el descriptor de fichero

    switch (pid = fork()) // Creamos un hijo y guardamos su PID
    {
    case -1: // Error en fork
        perror("fork()"); // Informamos del error
        return -1;        // Retornamos -1 a main para indicar el error
    case 0:               // Estamos en el Proceso HIJO
        // Redirigimos la entrada estándar (STDIN_FILENO, descriptor 0)
        [cite_start]// Abrimos el fichero de entrada (lado_dcho) en modo solo lectura (O_RDONLY) [cite: 39]
        if ((fd = open(lado_dcho, O_RDONLY)) == -1)
        {
            perror("open(fd_entrada)"); // Si falla (ej. fichero no existe)
            exit(EXIT_FAILURE);         // El hijo termina con error
        }
        // Duplicamos el descriptor del fichero (fd) sobre STDIN_FILENO (0)
        if (dup2(fd, STDIN_FILENO) == -1)
        {
            perror("dup2(stdin)"); // Si falla el dup2
            exit(EXIT_FAILURE);   // El hijo termina
        }
        // Cerramos el descriptor original (fd). Ya no lo necesitamos, STDIN_FILENO apunta al fichero.
        if (close(fd) == -1)
        {
            perror("close(fd_entrada)");
            exit(EXIT_FAILURE);
        }

        // Ejecutamos el comando (lado_izq)
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE); // No debería llegar aquí (execvp reemplaza el proceso)
        break;

    default: // Estamos en el Proceso PADRE
        // El padre simplemente retorna el PID del hijo que acaba de crear
        return pid;
    }
}

[cite_start]// Caso: comando > fichero_salida o comando >> fichero_salida [cite: 12, 13]
// 'doble' será true si es >>, false si es >
pid_t redireccion_dcha_o_doble_fork(char *lado_izq, char *lado_dcho, bool doble)
{
    pid_t pid; // Variable para el PID
    int fd;    // Variable para el descriptor de fichero
    int flags; // Banderas para la llamada open()

    [cite_start]// Determinamos las flags para open() según sea > (TRUNC) o >> (APPEND) [cite: 39]
    if (doble) // Si es >>
    {
        // Escribir (O_WRONLY), Crear si no existe (O_CREAT), Añadir al final (O_APPEND)
        flags = O_WRONLY | O_CREAT | O_APPEND;
    }
    else // Si es >
    {
        // Escribir (O_WRONLY), Crear si no existe (O_CREAT), Truncar a 0 si existe (O_TRUNC)
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    }

    switch (pid = fork()) // Creamos el hijo
    {
    case -1: // Error en fork
        perror("fork()");
        return -1;
    case 0: // Proceso HIJO
        // Redirigimos la salida estándar (STDOUT_FILENO, descriptor 1)
        // Abrimos el fichero (lado_dcho) con las flags y permisos 0664 (rw-rw-r--)
        if ((fd = open(lado_dcho, flags, 0664)) == -1)
        {
            perror("open(fd_salida)"); // Si falla (ej. permisos)
            exit(EXIT_FAILURE);
        }
        // Duplicamos el descriptor del fichero (fd) sobre STDOUT_FILENO (1)
        if (dup2(fd, STDOUT_FILENO) == -1)
        {
            perror("dup2(stdout)");
            exit(EXIT_FAILURE);
        }
        // Cerramos el descriptor original (fd)
        if (close(fd) == -1)
        {
            perror("close(fd_salida)");
            exit(EXIT_FAILURE);
        }

        // Ejecutamos el comando (lado_izq)
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE); // No debería llegar aquí
        break;

    default: // Proceso PADRE
        // El padre retorna el PID del hijo
        return pid;
    }
}

[cite_start]// 9. Ejecución: comando simple (sin operador) [cite: 9]
pid_t sin_operadores_fork(char *lado_izq)
{
    pid_t pid; // Variable para el PID

    switch (pid = fork()) // Creamos el hijo
    {
    case -1: // Error
        perror("fork()");
        return -1;
    case 0: // Proceso HIJO
        // CRÍTICO: Llamamos a la función para redirigir stderr a /dev/null
        // Esto evita que errores del hijo (ej. "ls: cannot access 'notest'...")
        // se muestren en la terminal.
        redir_stderr_to_null(); 
        
        // Ejecutamos el comando
        ejecutar_comando(lado_izq);
        exit(EXIT_FAILURE); // No debería llegar aquí
        break;
    default: // Proceso PADRE
        return pid; // Retornamos el PID
    }
}
// NOTA: Esta línea (redir_stderr_to_null();) debe añadirse de forma similar en los case 0: de TODAS las funciones de fork (redireccion_izq, redireccion_dcha, y en AMBOS hijos de tuberia_exec).
// (Nota del explicador: Tu código solo lo tiene aquí. Para cumplir esta nota,
// deberías añadir 'redir_stderr_to_null();' en 'redireccion_izq_fork' y
// 'redireccion_dcha_o_doble_fork', justo después del 'case 0:')


// 9. Función para chequear el estado de un hijo (NORMALIZACIÓN DEL CÓDIGO DE ERROR)
// Esta función es llamada por el PADRE después de un wait() o waitpid().
// 'status' es el valor devuelto por wait(), 'num_linea' es la línea que ejecutó ese hijo.
void chequear_status(int status, int num_linea)
{
    // Si ya hemos registrado un error (codigo_salida_global != 0),
    // simplemente ignoramos este (la tarea pide parar tras el *primer* error).
    if (codigo_salida_global != 0) return;

    // WIFEXITED(status) es true si el hijo terminó "normalmente" (llamando a exit())
    if (WIFEXITED(status))
    {
        // WEXITSTATUS(status) obtiene el código de salida (ej. 0 para éxito, 1 para error)
        int codigo_salida_real = WEXITSTATUS(status);
        if (codigo_salida_real != 0) // Si el hijo terminó con un código de error (NO cero)
        {
            [cite_start]// Imprimimos el mensaje de error EXACTO que pide el PDF [cite: 110]
            fprintf(stderr, "Error al ejecutar la línea %d. Terminación normal con el código 1.\n", num_linea);
            
            parar_ejecucion_flag = 1; // Activamos la bandera para no lanzar más procesos
            codigo_salida_global = 1; // Guardamos 1 como código de salida final
        }
    }
    // WIFSIGNALED(status) es true si el hijo fue terminado por una señal (ej. kill)
    else if (WIFSIGNALED(status))
    {
        // WTERMSIG(status) obtiene el número de la señal que mató al proceso
        int signal_num = WTERMSIG(status);
        
        [cite_start]// Imprimimos el mensaje de error EXACTO que pide el PDF [cite: 116]
        fprintf(stderr, "Error al ejecutar la línea %d. Terminación anormal por señal %d.\n", num_linea, signal_num);
        
        parar_ejecucion_flag = 1; // Activamos la bandera
        codigo_salida_global = 1; // Guardamos 1 como código de salida final
    }
}

// 10. Función de ejecución para Tuberías (gestión interna)
[cite_start]// Caso: comando1 | comando2 [cite: 14]
[cite_start]// Esta función crea DOS hijos y los conecta con un pipe. [cite: 40]
void tuberia_exec(char *lado_izq, char *lado_dcho, int num_linea)
{
    pid_t pid_izq, pid_dcho; // PIDs para los dos hijos
    int pipefds[2];          // Array para los descriptores del pipe: [0] = lectura, [1] = escritura
    int status_izq, status_dcho; // Para guardar el estado de salida de cada hijo

    // Creamos la tubería
    if (pipe(pipefds) == -1)
    {
        perror("pipe()");
        parar_ejecucion_flag = 1; // Error grave, activamos la bandera
        if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE; // Guardamos error
        return; // Salimos de la función
    }

    // --- Hijo Izquierdo (escritor) ---
    switch (pid_izq = fork()) // Creamos el primer hijo
    {
    case -1: // Error
        perror("fork() izq");
        parar_ejecucion_flag = 1;
        if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
        close(pipefds[0]); // Cerramos ambos extremos del pipe
        close(pipefds[1]);
        return; // Salimos
    case 0: // HIJO IZQUIERDO
        close(pipefds[0]); // Cierra el extremo de lectura (no lo usa)

        // Redirige su stdout (1) al extremo de escritura del pipe (pipefds[1])
        if (dup2(pipefds[1], STDOUT_FILENO) == -1)
        {
            perror("dup2() izq");
            exit(EXIT_FAILURE);
        }
        close(pipefds[1]); // Cierra el descriptor original

        // (Aquí faltaría la llamada a redir_stderr_to_null() si quisieras)
        ejecutar_comando(lado_izq); // Ejecuta el comando (ej. ls)
        exit(EXIT_FAILURE); // No debería llegar
    default: // PADRE
        break; // El padre continúa para crear el segundo hijo
    }

    // --- Hijo Derecho (lector) ---
    switch (pid_dcho = fork()) // Creamos el segundo hijo
    {
    case -1: // Error
        perror("fork() dcho");
        parar_ejecucion_flag = 1;
        if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
        // Intentamos "matar" al hijo izquierdo que ya lanzamos, para limpiar
        if (pid_izq > 0) kill(pid_izq, SIGKILL);
        close(pipefds[0]); // Cerramos pipes
        close(pipefds[1]);
        return;
    case 0: // HIJO DERECHO
        close(pipefds[1]); // Cierra el extremo de escritura (no lo usa)

        // Redirige su stdin (0) al extremo de lectura del pipe (pipefds[0])
        if (dup2(pipefds[0], STDIN_FILENO) == -1)
        {
            perror("dup2() dcho");
            exit(EXIT_FAILURE);
        }
        close(pipefds[0]); // Cierra el descriptor original

        // (Aquí faltaría la llamada a redir_stderr_to_null() si quisieras)
        ejecutar_comando(lado_dcho); // Ejecuta el comando (ej. wc -c)
        exit(EXIT_FAILURE); // No debería llegar
    default: // PADRE
        break; // El padre continúa para esperar
    }

    // --- Padre (espera) ---
    // ¡CRÍTICO! El padre debe cerrar AMBOS extremos del pipe.
    // Si no lo hace, el hijo derecho (lector) nunca recibirá EOF.
    close(pipefds[0]);
    close(pipefds[1]);

    // Esperamos específicamente al hijo izquierdo
    if (waitpid(pid_izq, &status_izq, 0) == -1)
    {
        perror("waitpid(izq)");
    }
    else
    {
        // Chequeamos si el hijo izquierdo falló
        chequear_status(status_izq, num_linea);
    }

    // Esperamos específicamente al hijo derecho
    if (waitpid(pid_dcho, &status_dcho, 0) == -1)
    {
        perror("waitpid(dcho)");
    }
    else
    {
        // Chequeamos si el hijo derecho falló
        chequear_status(status_dcho, num_linea);
    }
}

// 11. Función para procesar la línea (parsear)
// Esta función es el "cerebro" que analiza la línea y decide qué hacer.
// Retorna:
// > 0: PID del hijo creado (main debe rastrearlo)
//   0: Tubería (ya gestionada internamente) o error de parseo
//  -1: Error de fork (main debe parar)
pid_t procesar_linea(char *linea, int num_linea)
{
    char *lado_izq = linea; // Al principio, todo es "lado izquierdo"
    char *lado_dcho = NULL; // Aún no hay lado derecho
    char *operador = NULL;  // Aún no hay operador
    char *op_ptr = NULL;    // Puntero a la *posición* del operador
    operador_enum enum_op = NADA; // Por defecto, no hay operador
    bool doble = false; // Flag para '>>'

    [cite_start]// Buscamos operadores en orden de precedencia (>> antes que >) [cite: 37]
    // strstr busca un *string* (subcadena)
    if ((op_ptr = strstr(linea, ">>")) != NULL)
    {
        operador = ">>";  // El operador es ">>"
        doble = true;    // Activamos la bandera 'doble'
        enum_op = REDIR2; // Guardamos el tipo de operador
    }
    // strchr busca un *carácter*
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

    // Si encontramos un operador (cualquiera menos NADA)
    if (enum_op != NADA)
    {
        *op_ptr = '\0'; // Cortamos el string: ponemos un NULO donde empieza el operador
                        // Ahora 'lado_izq' (que apunta a 'linea') termina aquí.
        // El lado derecho empieza *después* del operador
        lado_dcho = op_ptr + strlen(operador);

        // Limpiamos espacios de ambos lados
        lado_izq = trim_whitespace(lado_izq);
        lado_dcho = trim_whitespace(lado_dcho);

        [cite_start]// Comprobación de error: ¿hay más de un operador? [cite: 38]
        // (Buscamos si en el lado_dcho hay *otro* operador)
        if (strchr(lado_dcho, '>') || strchr(lado_dcho, '<') || strchr(lado_dcho, '|')) {
             // Si hay, imprimimos error
             fprintf(stderr, "Error: línea %d contiene más de un operador.\n", num_linea);
             parar_ejecucion_flag = 1; // Activamos bandera de parar
             if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE; // Guardamos error
             return 0; // 0 para que main no rastree PID (no se lanzó nada)
        }
    }
    else // Si no hubo operador
    {
        lado_izq = trim_whitespace(linea); // Solo limpiamos el "lado izquierdo" (la línea entera)
    }


    // Llamamos a la función de ejecución correspondiente
    switch (enum_op)
    {
    case REDIR2: // Caso >>
        // Llamamos a la función de fork y retornamos el PID que nos dé
        return redireccion_dcha_o_doble_fork(lado_izq, lado_dcho, doble); // 'doble' es true
    case REDIR_DCHA: // Caso >
        // Llamamos a la función de fork y retornamos el PID que nos dé
        return redireccion_dcha_o_doble_fork(lado_izq, lado_dcho, doble); // 'doble' es false
    case REDIR_IZQ: // Caso <
        // Llamamos a la función de fork y retornamos el PID que nos dé
        return redireccion_izq_fork(lado_izq, lado_dcho);
    case TUBERIA: // Caso |
        // Llamamos a la función de tubería (esta función gestiona sus propios waitpid)
        tuberia_exec(lado_izq, lado_dcho, num_linea);
        return 0; // Retornamos 0 para que main sepa que NO debe rastrear un PID
    default: // Caso NADA
        // Llamamos a la función de fork simple y retornamos el PID
        return sin_operadores_fork(lado_izq);
    }
}

// 12. Función para procesar un hijo que ha terminado
// Llamada por main cuando wait() retorna un PID.
// 'pids_activos' es el array donde rastreamos los hijos.
void procesar_hijo_terminado(pid_t pid, int status, struct ProcesoHijo *pids_activos, int max_procs)
{
    if (pid <= 0) return; // Si wait() dio error (ej. -1), no hacemos nada

    // Buscamos el PID que acaba de terminar en nuestro array
    for (int i = 0; i < max_procs; i++)
    {
        // Si encontramos el PID en la ranura 'i'
        if (pids_activos[i].pid == pid)
        {
            // Recuperamos el número de línea que estaba ejecutando
            int num_linea_terminada = pids_activos[i].num_linea;

            // Comprobamos si terminó con error
            chequear_status(status, num_linea_terminada);

            // Liberamos la ranura
            pids_activos[i].pid = 0; // La ponemos a 0 (libre)
            pids_activos[i].num_linea = 0;
            procs_en_ejecucion--; // Decrementamos el contador global de procesos
            return; // Salimos de la función (ya encontramos y procesamos el PID)
        }
    }
    // Si el PID no se encuentra, puede ser un hijo de una tubería
    // (que no rastreamos en main), así que lo ignoramos.
}

// 13. Función de ayuda (Uso)
[cite_start]// Muestra el mensaje de ayuda [cite: 49-57]
void mostrar_uso(char *prog_name)
{
    // 'stderr' es la salida de error estándar. La ayuda suele ir aquí.
    fprintf(stderr, "Uso: %s [-b BUF_SIZE] [-l MAX_LINE_SIZE] [-p NUM_PROCS]\n", prog_name);
    // 'printf' (que es 'stdout') para el resto del mensaje, como en el ejemplo.
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
    int opt; // Variable para almacenar la opción de getopt
    // Valores iniciales (por defecto)
    int buf_size = BUF_SIZE_DEF;         [cite_start]// [cite: 26]
    int max_line_size = MAX_LINE_SIZE_DEF; [cite_start]// [cite: 26]
    int num_procs = NUM_PROCS_DEF;       [cite_start]// [cite: 27]

    char *buffer; [cite_start]// Puntero al buffer para leer de read() [cite: 35]
    char *linea;  // Puntero al buffer para ensamblar la línea actual

    [cite_start]// Parseo de argumentos con getopt() [cite: 25]
    // "b:l:p:h" -> b, l, y p esperan un argumento (los ':'). h no.
    while ((opt = getopt(argc, argv, "b:l:p:h")) != -1)
    {
        switch (opt) // 'opt' contiene la letra de la opción (ej. 'b')
        {
        case 'b':
            buf_size = atoi(optarg); // 'optarg' apunta al argumento (ej. "128"). atoi lo convierte a int.
            break;
        case 'l':
            max_line_size = atoi(optarg);
            break;
        case 'p': 
            num_procs = atoi(optarg);
            break;
        case 'h': // Opción de ayuda
            mostrar_uso(argv[0]); // argv[0] es el nombre del programa (ej. "./exec_lines")
            exit(EXIT_SUCCESS);   // Salimos con éxito
        default: // Error en opción (ej. -x) o falta argumento (ej. -b)
            mostrar_uso(argv[0]);
            exit(EXIT_FAILURE); // Salimos con error
        }
    }

    // Validación de argumentos (comprobamos los rangos)
    if (buf_size < BUF_MIN || buf_size > BUF_MAX)
    {
        fprintf(stderr, "Error: El tamaño de buffer tiene que estar entre %d y %d.\n", BUF_MIN, BUF_MAX); 
        exit(EXIT_FAILURE); // Salimos con error
    }
    if (max_line_size < LINE_MIN || max_line_size > LINE_MAX)
    {
        fprintf(stderr, "Error: El tamaño de línea tiene que estar entre %d y %d.\n", LINE_MIN, LINE_MAX);
        exit(EXIT_FAILURE);
    }
    if (num_procs < PROC_MIN || num_procs > PROC_MAX)
    {
        fprintf(stderr, "Error: El número de procesos en ejecución tiene que estar entre %d y %d.\n", PROC_MIN, PROC_MAX);
        mostrar_uso(argv[0]); [cite_start]// Mostramos la ayuda también [cite: 81]
        exit(EXIT_FAILURE);
    }

    // --- Reserva de Memoria ---
    // Reservamos 'buf_size' bytes para el buffer de lectura
    if ((buffer = (char *)malloc(buf_size * sizeof(char))) == NULL)
    {
        perror("malloc(buffer)");
        exit(EXIT_FAILURE);
    }
    // Reservamos 'max_line_size + 2' para la línea
    // +1 para el '\0'
    // +1 para poder detectar el desbordamiento (si indice_linea == max_line_size)
    if ((linea = (char *)malloc((max_line_size + 2) * sizeof(char))) == NULL)
    {
        perror("malloc(linea)");
        free(buffer); // Liberamos el buffer antes de salir
        exit(EXIT_FAILURE);
    }

    // Array para rastrear los PIDs de los hijos paralelos
    // Declaramos un array (en la pila) de 'num_procs' elementos de tipo 'struct ProcesoHijo'
    struct ProcesoHijo pids_activos[num_procs];
    // Inicializamos el array
    for (int i = 0; i < num_procs; i++)
    {
        pids_activos[i].pid = 0; // 0 = slot libre
        pids_activos[i].num_linea = 0;
    }

    ssize_t num_leidos;   // Para guardar cuántos bytes ha leído 'read()'
    int indice_linea = 0; // Dónde estamos escribiendo en el buffer 'linea'
    int num_linea = 1;    // Contador de líneas (para los mensajes de error)

    // --- Bucle Principal de Lectura ---
    [cite_start]// Lee 'buf_size' bytes de la entrada estándar (STDIN_FILENO) y los guarda en 'buffer' [cite: 35]
    // El bucle continúa mientras 'read()' devuelva > 0 (bytes leídos)
    // Si devuelve 0, es Fin de Fichero (EOF). Si devuelve -1, es un error.
    while ((num_leidos = read(STDIN_FILENO, buffer, buf_size)) > 0)
    {
        // Procesa el buffer leído caracter a caracter
        for (int i = 0; i < num_leidos; i++)
        {
            [cite_start]// Comprobación de desbordamiento de línea [cite: 36]
            // Si 'indice_linea' alcanza el tamaño máximo...
            if (indice_linea >= max_line_size)
            {
                // ...la línea es demasiado larga.
                // Copiamos para asegurar que está terminada en NULL e imprimir.
                linea[max_line_size] = '\0';
                [cite_start]// Imprimimos el error [cite: 93, 101]
                fprintf(stderr, "Error, línea %d demasiado larga: \"%s...\" \n", num_linea, linea);
                parar_ejecucion_flag = 1; // Activamos bandera
                if (codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE; // Guardamos error
                
                // Ahora debemos descartar el resto de la línea (hasta el \n)
                // Bucle: mientras estemos dentro del buffer Y no sea \n...
                while (i < num_leidos && buffer[i] != '\n') {
                    i++; // ...avanzamos 'i', descartando el carácter
                }
                
                // Si encontramos el '\n' (es decir, buffer[i] == '\n')
                if (buffer[i] == '\n') {
                    indice_linea = 0; // Reseteamos el índice de línea
                    num_linea++;      // Pasamos a la siguiente línea
                }
                // Si no encontramos '\n' (se acabó el buffer), el 'for' terminará,
                // 'read()' leerá más, y volveremos a entrar en este 'if' (porque
                // 'indice_linea' sigue siendo >= max_line_size) hasta que encontremos el '\n'.
                
                // Salimos del bucle 'for' (no procesamos más este buffer)
                // pero NO del 'while (read)'
                break; 
            }

            // (Si la línea NO es demasiado larga)
            // Añadimos el caracter del buffer de lectura a nuestra línea temporal
            linea[indice_linea] = buffer[i];

            // ¿Es el fin de línea?
            if (buffer[i] == '\n')
            {
                linea[indice_linea] = '\0'; // Terminamos el string (reemplazamos \n por \0)

                // --- Gestión de Concurrencia ---
                
                // 1. ¿Estamos al máximo de procesos?
                // Si 'procs_en_ejecucion' ha alcanzado el límite 'num_procs'
                // Y NO estamos en modo "parar por error"...
                [cite_start]// Si num_procs es 1, esto se ejecuta siempre, forzando espera (secuencial) [cite: 29]
                while (procs_en_ejecucion >= num_procs && !parar_ejecucion_flag)
                {
                    int status; // Variable para el estado del hijo
                    // wait(-1, ...) espera a CUALQUIER hijo que termine.
                    // Esta llamada es BLOQUEANTE.
                    pid_t pid_terminado = wait(&status);
                    if (pid_terminado > 0) // Si wait() funcionó
                    {
                        // Procesamos el hijo que terminó (chequear errores, liberar slot)
                        procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
                    }
                }

                // 2. ¿Debemos parar?
                [cite_start]// Si un proceso anterior falló (el flag está activo), no lanzamos más. [cite: 44]
                if (parar_ejecucion_flag)
                {
                    // Rompemos el bucle 'for' para dejar de procesar el buffer actual
                    break;
                }
                
                // 3. Lanzar el nuevo proceso
                // Llamamos al "cerebro" para que parsee y ejecute la línea
                pid_t nuevo_pid = procesar_linea(linea, num_linea);

                if (nuevo_pid > 0) // Si > 0, fue un comando simple/redirección y tenemos un PID
                {
                    // Debemos rastrearlo
                    bool slot_encontrado = false; // Flag para saber si lo hemos guardado
                    // Buscamos un slot libre (pid == 0) en pids_activos
                    for (int j = 0; j < num_procs; j++)
                    {
                        if (pids_activos[j].pid == 0)
                        {
                            pids_activos[j].pid = nuevo_pid;     // Guardamos el PID
                            pids_activos[j].num_linea = num_linea; // Guardamos su nº de línea
                            procs_en_ejecucion++;                // Incrementamos el contador
                            slot_encontrado = true;              // Marcamos como guardado
                            break; // Salimos del 'for' (ya encontramos slot)
                        }
                    }
                    if (!slot_encontrado) { // Esto NO debería pasar
                        fprintf(stderr, "Error interno: no hay slots libres para PIDs.\n");
                        parar_ejecucion_flag = 1;
                        if(codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
                    }
                }
                else if (nuevo_pid < 0) // Si < 0, fue un error de fork()
                {
                    perror("fork() en procesar_linea"); // (procesar_linea ya imprimió 'perror' pero por si acaso)
                    parar_ejecucion_flag = 1;
                    if(codigo_salida_global == 0) codigo_salida_global = EXIT_FAILURE;
                }
                // Si nuevo_pid == 0, fue una tubería (ya gestionada) o un error de parseo
                // (ya gestionado). No hacemos nada.

                // Preparamos para la siguiente línea
                num_linea++;       // Incrementamos el contador de línea
                indice_linea = 0;  // Reseteamos el índice del buffer 'linea'
            }
            else // Si no es '\n'
            {
                // Seguimos añadiendo caracteres
                indice_linea++;
            }
        } // fin for (procesar buffer)

        // Si se activó el flag de error (ej. línea larga, error de hijo),
        // salimos también del bucle de lectura 'while (read)'
        if (parar_ejecucion_flag)
        {
            break;
        }
    } // fin while (read)

    // Si 'num_leidos' fue -1, 'read()' falló
    if (num_leidos == -1)
    {
        perror("read()");
    }


    //insercion de un codigo para que funcione el cat del echo -e -n
    // (Este código maneja el caso de que el fichero termine SIN un '\n' final)
    
    // Si 'indice_linea' > 0, significa que nos han quedado caracteres en el
    // buffer 'linea' cuando 'read()' devolvió 0 (EOF).
    if (indice_linea > 0) {
        // La terminamos manualmente con NULL.
        linea[indice_linea] = '\0';
        
        // Antes de procesarla, hacemos la misma comprobación de concurrencia:
        // esperamos a que haya un slot libre si estamos llenos.
        while (procs_en_ejecucion >= num_procs && !parar_ejecucion_flag) {
            int status;
            pid_t pid_terminado = wait(&status);
            if (pid_terminado > 0)
                procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
        }

        // Procesamos esta última línea (si no ha habido un error previo)
        if (!parar_ejecucion_flag) {
            pid_t nuevo_pid = procesar_linea(linea, num_linea);
            
            // Lógica de registro de PID (igual que en el bucle principal)
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
    // procesos ejecutándose (ej. con -p 3 y 2 sleeps)
    [cite_start]// Debemos esperar a que TODOS terminen. [cite: 44]
    // Bucle: mientras queden procesos en ejecución...
    while (procs_en_ejecucion > 0)
    {
        int status;
        // Esperamos a que termine *cualquier* hijo
        pid_t pid_terminado = wait(&status);
        if (pid_terminado > 0) // Si wait() funcionó
        {
            // Procesamos el hijo (chequear errores, decrementar contador)
            procesar_hijo_terminado(pid_terminado, status, pids_activos, num_procs);
        }
    }

    // Liberamos la memoria dinámica que pedimos con malloc
    free(buffer);
    free(linea);

    // Salimos con el código de salida del primer error (que será 1)
    // o con 0 si 'codigo_salida_global' nunca se modificó (todo fue bien).
    exit(codigo_salida_global);
}