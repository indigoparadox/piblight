
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <termios.h>
#include <string.h>
#include <stdlib.h>
#include <mosquitto.h>
#include <assert.h>
#include <stdint.h>

#define MQTT_ITEM_SZ 255
#define CONFIG_PATH_SZ 255
#define LINE_BUF_SZ 255

#define BRIGHTNESS_PATH "/sys/class/backlight/rpi_backlight/brightness"

#define BUF_TYPE_STR 0
#define BUF_TYPE_INT 1

#define debug_printf_3( ... ) printf( __VA_ARGS__ )
#define debug_printf_2( ... ) printf( __VA_ARGS__ )
#if defined( DEBUG )
#  define debug_printf_1( ... ) printf( __VA_ARGS__ )
#else
#  define debug_printf_1( ... )
#endif
#define error_printf( ... ) fprintf( stderr, __VA_ARGS__ )

int g_retval = 0;

char g_cfg_path[CONFIG_PATH_SZ] = "piblight.conf";

size_t cfg_read(
   const char* cfg_path, const char* sect, const char* key, int idx,
   int buf_type, void* buf_out, size_t buf_out_sz
) {
   char c,
      key_buf[LINE_BUF_SZ + 1],
      val_buf[LINE_BUF_SZ + 1],
      sect_buf[LINE_BUF_SZ + 1];
   int cfg = 0,
      in_val = 1,
      in_sect = 0,
      * int_out = NULL;
   size_t read_sz = 0,
      line_total = 0,
      idx_iter = 0;

   memset( buf_out, '\0', buf_out_sz );

   cfg = open( cfg_path, O_RDONLY );

   /* TODO: Check for open failure! */

   do {
      line_total = 0;
      in_val = 0;
      memset( key_buf, '\0', LINE_BUF_SZ + 1 );
      memset( val_buf, '\0', LINE_BUF_SZ + 1 );

      /* Read characters into line buffer. */
      do {
         read_sz = read( cfg, &c, 1 );
         if( 1 > read_sz ) {
            goto cleanup;

         } else if( '[' == c ) {
            line_total = 0;
            in_sect = 1;
            in_val = 0;
            idx_iter = 0; /* Index is only per-section. */

         } else if( ']' == c ) {
            sect_buf[line_total] = 0;
            line_total = 0;
            in_sect = 0;

         } else if( '=' == c ) {
            /* We're done reading the key and moving to the value. */
            line_total = 0;
            in_val = 1;

         } else if( '\n' == c ) {
            /* We're reached the end of the line. */
            break;

         } else if( in_sect ) {
            /* We're reading the section. */
            sect_buf[line_total++] = c;

         } else if( in_val ) {
            /* We're reading the value. */
            val_buf[line_total++] = c;

         } else {
            /* We're still reading the key. */
            key_buf[line_total++] = c;
         }
      } while( read_sz == 1 );

      if(
         0 == strncmp( sect, sect_buf, strlen( sect ) ) &&
         0 == strncmp( key, key_buf, strlen( key ) ) &&
         idx_iter == idx
      ) {
         switch( buf_type ) {
         case BUF_TYPE_STR:
            strncpy( buf_out, val_buf, buf_out_sz );
            return strlen( val_buf );

         case BUF_TYPE_INT:
            assert( sizeof( int ) == buf_out_sz );
            int_out = (int*)buf_out;
            *int_out = atoi( val_buf );
            return sizeof( int );

         default:
            return 0;
         }
      } else if( 0 == strncmp( key, key_buf, strlen( key ) ) ) {
         /* Found a match, but not the index requested! */
         idx_iter++;
      }
   } while( 1 );

cleanup:
   return 0;
}

void publish_brightness( struct mosquitto* mqtt ) {
   char topic_buffer[MQTT_ITEM_SZ + 1];
   char brightness_buffer[LINE_BUF_SZ + 1];
   int rc = 0;
   int brightness_fd = 0;
   size_t brightness_buffer_sz = 0;

   cfg_read(
      g_cfg_path, "mqtt", "topic_get", 0,
      BUF_TYPE_STR, topic_buffer, MQTT_ITEM_SZ );

   brightness_fd = open( BRIGHTNESS_PATH, O_RDWR );
   /* TODO: Check for open failure! */

   brightness_buffer_sz =
      read( brightness_fd, &brightness_buffer, LINE_BUF_SZ );
   if( 0 == brightness_buffer_sz ) {
      error_printf( "error reading system brightness!\n" );
      goto cleanup;
   }

   debug_printf_2(
      "publishing %s to %s...\n", brightness_buffer, topic_buffer );
   rc = mosquitto_publish( mqtt, NULL, topic_buffer, brightness_buffer_sz,
      brightness_buffer, 0, 0 );
   if( MOSQ_ERR_SUCCESS != rc ) {
      error_printf(
         "error publishing %s to %s!\n", brightness_buffer, topic_buffer );
   }

cleanup:

   if( 0 < brightness_fd ) {
      close( brightness_fd );
   }
}

void on_connect( struct mosquitto* mqtt, void* obj, int reason_code ) {
   int rc = 0;
   char topic_buffer[MQTT_ITEM_SZ + 1];

   debug_printf_3( "MQTT connected\n" );

   cfg_read(
      g_cfg_path, "mqtt", "topic_set", 0,
      BUF_TYPE_STR, topic_buffer, MQTT_ITEM_SZ );

   debug_printf_2( "subscribing to %s...\n", topic_buffer );

   rc = mosquitto_subscribe( mqtt, NULL, topic_buffer, 1 );
   if( MOSQ_ERR_SUCCESS == rc ) {
      debug_printf_2( "subscribed\n" );
   } else {
      error_printf( "error subscribing\n" );
      mosquitto_disconnect( mqtt );
   }

   publish_brightness( mqtt );
}

void on_message(
   struct mosquitto* mqtt, void *obj, const struct mosquitto_message *msg
) {
   int brightness_fd = 0;
   char next_op = 0;
   float f_buf = 0;
   char* payload = msg->payload;
   char* topic = msg->topic;
   int retval = 0;

   debug_printf_1( "received message at %s: %s\n", topic, payload );

   brightness_fd = open( BRIGHTNESS_PATH, O_RDWR );

   /* TODO: Check for open failure! */

   if( 
      strlen( payload ) !=
      write( brightness_fd, payload, strlen( payload ) )
   ) {
      error_printf( "error writing to brightness path!\n" );
   }

   close( brightness_fd );

   publish_brightness( mqtt );

#if 0
   if( 0 /* TODO */ ) {
      error_printf( "stopping MQTT loop!\n" );
      g_retval = 1;
      mosquitto_disconnect( mqtt );
      mosquitto_loop_stop( mqtt, 0 );
   }
#endif
}

int main( int argc, char* argv[] ) {
   struct termios serset;
   struct mosquitto* mqtt = NULL;
   int rc = 0,
      ser_baud = 0,
      mqtt_port = 0;
   char mqtt_host[MQTT_ITEM_SZ + 1];
   char mqtt_user[MQTT_ITEM_SZ + 1];
   char mqtt_pass[MQTT_ITEM_SZ + 1];

   mosquitto_lib_init();

   mqtt = mosquitto_new( NULL, 1, NULL );
   if( NULL == mqtt ) {
      error_printf( "mosquitto init failure\n" );
      g_retval = 1; \
      goto cleanup;
   }

   if( 1 < argc ) {
      memset( g_cfg_path, '\0', CONFIG_PATH_SZ );
      strncpy( g_cfg_path, argv[1], strlen( argv[1] ) );
      debug_printf_3( "using config: %s\n", g_cfg_path );
   }

   mosquitto_connect_callback_set( mqtt, on_connect );
   mosquitto_message_callback_set( mqtt, on_message );

   /* Setup MQTT connection. */
   cfg_read(
      g_cfg_path, "mqtt", "host", 0, BUF_TYPE_STR, mqtt_host, MQTT_ITEM_SZ );
   cfg_read(
      g_cfg_path, "mqtt", "port", 0, BUF_TYPE_INT, &mqtt_port, sizeof( int ) );
   cfg_read(
      g_cfg_path, "mqtt", "user", 0, BUF_TYPE_STR, mqtt_user, MQTT_ITEM_SZ );
   cfg_read(
      g_cfg_path, "mqtt", "pass", 0, BUF_TYPE_STR, mqtt_pass, MQTT_ITEM_SZ );

   debug_printf_3(
      "connecting to %s:%d as %s...\n", mqtt_host, mqtt_port, mqtt_user );

   mosquitto_username_pw_set( mqtt, mqtt_user, mqtt_pass );

   rc = mosquitto_connect( mqtt, mqtt_host, mqtt_port, 60 );
   if( MOSQ_ERR_SUCCESS != rc ) {
      g_retval = 1;
      mosquitto_destroy( mqtt );
      error_printf( "MQTT error: %s\n", mosquitto_strerror( rc ) );
      goto cleanup;
   }

   mosquitto_loop_forever( mqtt, -1, 1 );

cleanup:

   mosquitto_lib_cleanup();

   return g_retval; 
}

