/*
 * log.c
 * Logging facilities.
 *
 * Matej Pfajfar <mp292@cam.ac.uk>
 */

/*
 * Changes :
 * $Log$
 * Revision 1.3  2002/09/04 00:39:33  arma
 * the logs now include a timestamp and severity
 *
 * the implementation is sort of a kludge..you're welcome to fix it up
 *
 * Revision 1.2  2002/07/12 18:14:16  montrose
 * removed loglevel from global namespace. severity level is set using log() with a NULL format argument now. example: log(LOG_ERR,NULL);
 *
 * Revision 1.1.1.1  2002/06/26 22:45:50  arma
 * initial commit: current code
 *
 * Revision 1.11  2002/06/14 20:44:57  mp292
 * *** empty log message ***
 *
 * Revision 1.10  2002/03/12 23:31:36  mp292
 * *** empty log message ***
 *
 * Revision 1.9  2002/03/02 18:55:50  mp292
 * LOG_DEBUG messages don't print the last errno error anymore.
 *
 * Revision 1.8  2002/01/26 22:46:48  mp292
 * Reviewd according to Secure-Programs-HOWTO.
 *
 * Revision 1.7  2002/01/17 15:00:43  mp292
 * Fixed a bug which caused malloc() generate a seg fault.
 *
 * Revision 1.6  2001/12/12 16:02:55  badbytes
 * Minor changes in output format.
 *
 * Revision 1.5  2001/12/12 06:48:07  badbytes
 * Correction - last error message now only shown if severity==LOG_DEBUG.
 *
 * Revision 1.4  2001/12/12 06:28:46  badbytes
 * Modified log() to print error message for last error in addition to the user-specified message.
 *
 * Revision 1.3  2001/12/07 09:38:03  badbytes
 * Tested.
 *
 * Revision 1.2  2001/12/06 15:43:50  badbytes
 * config.c compiles. Proceeding to test it.
 *
 * Revision 1.1  2001/11/21 23:03:41  mp292
 * log function coded and tested.
 * Top-level makefile.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include "log.h"

/* FIXME this whole thing is hacked together. feel free to make it clean. */
size_t sev_to_string(char *buf, int max, int severity) {
  assert(max > 20);

  switch(severity) {
    case LOG_DEBUG:
      strcpy(buf,"debug");  
      break;
    case LOG_INFO:
      strcpy(buf,"info");
      break;
    case LOG_NOTICE:
      strcpy(buf,"notice");
      break;
    case LOG_WARNING:
      strcpy(buf,"warn");
      break;
    case LOG_ERR:
      strcpy(buf,"err");
      break;
    case LOG_CRIT:
      strcpy(buf,"crit");
      break;
    case LOG_ALERT:
      strcpy(buf,"alert");
      break;
    case LOG_EMERG: 
      strcpy(buf,"emerg");
      break;
    default:
      strcpy(buf,"UNKNOWN");
      break;
  }

  return strlen(buf)+1;
}

/* Outputs a message to stdout */
void log(int severity, const char *format, ...)
{
  static int loglevel = LOG_DEBUG;
  char buf[201];
  time_t t;
  va_list ap;

  if ( format )
  {

    va_start(ap,format);
  
    if (severity <= loglevel)
    {
      t = time(NULL);
      strftime(buf, 200, "%b %d %H:%M:%S", localtime(&t));
      printf("%s ", buf);
      sev_to_string(buf, 200, severity);
      printf("[%s] ", buf);
      vprintf(format,ap);
      printf("\n");
    }
    
    va_end(ap);
  }
  else
    loglevel = severity;
}

