#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

int main()
{
   char cadena[] = "file1,file2,file3";
   char *ptrToken;
   char *saveptr;

   ptrToken = strtok_r( cadena, ",", &saveptr);
   while ( ptrToken != NULL ) {
      printf( "%s\n", ptrToken );
      ptrToken = strtok_r( NULL, ",", &saveptr );
   }
   return 0;
}
