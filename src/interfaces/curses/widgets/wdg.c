/*
    WDG -- widgets helper for ncurses

    Copyright (C) ALoR

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.

    $Id: wdg.c,v 1.6 2003/10/22 20:36:25 alor Exp $
*/

#include <wdg.h>

#include <ncurses.h>

/* not defined in curses.h */
#define KEY_TAB   '\t'

/* GLOBALS */

/* informations about the current screen */
struct wdg_scr current_screen;
/* called when idle */
static void (*wdg_idle_callback)(void);
/* the root object (usually the menu) */
static struct wdg_object *wdg_root_obj;
/* the focus list */
struct wdg_obj_list {
   struct wdg_object *wo;
   CIRCLEQ_ENTRY(wdg_obj_list) next;
};
static CIRCLEQ_HEAD(, wdg_obj_list) wdg_objects_list = CIRCLEQ_HEAD_INITIALIZER(wdg_objects_list);
/* the currently focused object */
static struct wdg_obj_list *wdg_focused_obj;

/* PROTOS */

void wdg_init(void);
void wdg_cleanup(void);
static void wdg_resize(void);

int wdg_events_handler(int exit_key);
void wdg_set_idle_callback(void (*callback)(void));
static void wdg_dispatch_msg(int key);
static void wdg_switch_focus(void);

int wdg_create_object(struct wdg_object **wo, size_t type, size_t flags);
int wdg_destroy_object(struct wdg_object **wo);

void wdg_resize_object(struct wdg_object *wo, int x1, int y1, int x2, int y2);
void wdg_draw_object(struct wdg_object *wo);
size_t wdg_get_type(struct wdg_object *wo);

/* creation function from other widgets */
extern void wdg_create_window(struct wdg_object *wo);

/*******************************************/

/*
 * init the widgets interface
 */
void wdg_init(void)
{
   /* initialize the curses interface */
   initscr(); 

   /* disable buffering until carriage return */
   cbreak(); 

   /* set the non-blocking timeout (10th of seconds) */
   halfdelay(WDG_INPUT_TIMEOUT);
   
   /* disable echo of typed chars */
   noecho();
  
   /* better compatibility with return key */
   nonl();

   /* don't flush input on break */
   intrflush(stdscr, FALSE);
  
   /* enable function and arrow keys */ 
   keypad(stdscr, TRUE);
  
   /* activate colors if available */
   if (has_colors()) {
      current_screen.flags |= WDG_SCR_HAS_COLORS;
      start_color();
   }

   /* hide the cursor */
   curs_set(FALSE);

   /* remember the current screen size */
   current_screen.lines = LINES;
   current_screen.cols = COLS;

   /* the wdg is initialized */
   current_screen.flags |= WDG_SCR_INITIALIZED;

   /* clear the screen */
   clear();

   /* sync the virtual and the physical screen */
   refresh();
}


/*
 * cleanup the widgets interface
 */
void wdg_cleanup(void)
{
   /* can only cleanup if it was initialized */
   if (!(current_screen.flags & WDG_SCR_INITIALIZED))
      return;
   
   /* show the cursor */
   curs_set(TRUE);

   /* clear the screen */
   clear();

   /* do the refresh */
   refresh();

   /* end the curses interface */
   endwin();

   /* wdg is not initialized */
   current_screen.flags &= ~WDG_SCR_INITIALIZED;
}


/* 
 * called upone screen resize
 */
static void wdg_resize(void)
{
   struct wdg_obj_list *wl;
   
   /* remember the current screen size */
   current_screen.lines = LINES;
   current_screen.cols = COLS;

   /* call the redraw function to all the objects */
   CIRCLEQ_FOREACH(wl, &wdg_objects_list, next) {
      WDG_BUG_IF(wl->wo->redraw == NULL);
      WDG_EXECUTE(wl->wo->redraw, wl->wo);
   }

   printw("WDG: size: %dx%d\n", LINES, COLS); refresh();
   
}

/*
 * this function handles all the inputed keys 
 * from the user and dispatches them to the
 * wdg objects
 */
int wdg_events_handler(int exit_key)
{
   int key;
   
   /* infinite loop */
   WDG_LOOP {

      /* get the input from user */
      key = wgetch(stdscr);

      switch (key) {
            
         case KEY_TAB:
            /* switch focus between objects */
            wdg_switch_focus();
            break;
           
         case KEY_RESIZE:
            /* the screen has been resized */
            wdg_resize();
            break;
              
         case ERR:
            /* 
             * non-blockin input reached the timeout:
             * call the idle function if present, else
             * sleep to not eat up all the cpu
             */
            if (wdg_idle_callback != NULL)
               wdg_idle_callback();
            else { 
               usleep(WDG_INPUT_TIMEOUT * 1000);
               /* XXX - too many refresh ? */
               refresh();
            }
            break;
            
         default:
            /* emergency exit key */
            if (key == exit_key)
               return WDG_ESUCCESS;
            
            /* dispatch the user input */
            wdg_dispatch_msg(key);
            break;
      }
   }
   
   /* NOT REACHED */
   
   return WDG_ESUCCESS;
}

/*
 * set the function to be called when idle 
 */
void wdg_set_idle_callback(void (*callback)(void))
{
   /* set the global pointer */
   wdg_idle_callback = callback;
}

/*
 * dispatch the user input to the list of objects.
 * first dispatch to the root_obj, if not handled
 * dispatch to the focused object.
 */
static void wdg_dispatch_msg(int key)
{
   /* first dispatch to the root object */
   if (wdg_root_obj != NULL) {
      if (wdg_root_obj->get_msg(wdg_root_obj, key) == WDG_ESUCCESS)
         /* the root object handled the message */
         return;
   }

   /* 
    * the root_object has not handled it.
    * dispatch to the focused one
    */
   if (wdg_focused_obj != NULL) {
      if (wdg_focused_obj->wo->get_msg(wdg_focused_obj->wo, key) == WDG_ESUCCESS)
         /* the root object handled the message */
         return;
   }
   
   /* reached if noone handle the message */
   
   printw("WDG: NOT HANDLED: char %d (%c)\n", key, (char)key); refresh();
}

/*
 * move the focus to the next object.
 * only WDG_OBJ_WANT_FOCUS could get the focus
 */
static void wdg_switch_focus(void)
{
   struct wdg_obj_list *wl;

   printw("WDG: switch focus\n"); refresh();
   
   /* if there is not a focused object, create it */
   if (wdg_focused_obj == NULL) {
   
      /* search the first "focusable" object */
      CIRCLEQ_FOREACH(wl, &wdg_objects_list, next) {
         if ((wl->wo->flags & WDG_OBJ_WANT_FOCUS) && (wl->wo->flags & WDG_OBJ_VISIBLE) ) {
            /* set the focused object */
            wdg_focused_obj = wl;
            /* focus current object */
            WDG_BUG_IF(wdg_focused_obj->wo->get_focus == NULL);
            WDG_EXECUTE(wdg_focused_obj->wo->get_focus, wdg_focused_obj->wo);
         }
      }
      return;
   }
  
   /* unfocus the current object */
   WDG_BUG_IF(wdg_focused_obj->wo->lost_focus == NULL);
   WDG_EXECUTE(wdg_focused_obj->wo->lost_focus, wdg_focused_obj->wo);
   
   /* 
    * focus the next element in the list.
    * only focus objects that have the WDG_OBJ_WANT_FOCUS flag
    */
   do {
      wdg_focused_obj = CIRCLEQ_NEXT(wdg_focused_obj, next);
      /* we are on the head, move to the first element */
      if (wdg_focused_obj == CIRCLEQ_END(&wdg_objects_list))
         wdg_focused_obj = CIRCLEQ_NEXT(wdg_focused_obj, next);
   } while ( !(wdg_focused_obj->wo->flags & WDG_OBJ_WANT_FOCUS) || !(wdg_focused_obj->wo->flags & WDG_OBJ_VISIBLE) );

   /* focus current object */
   WDG_BUG_IF(wdg_focused_obj->wo->get_focus == NULL);
   WDG_EXECUTE(wdg_focused_obj->wo->get_focus, wdg_focused_obj->wo);
   
}

/*
 * create a wdg object 
 */
int wdg_create_object(struct wdg_object **wo, size_t type, size_t flags)
{
   struct wdg_obj_list *wl;
   
   /* alloc the struct */
   WDG_SAFE_CALLOC(*wo, 1, sizeof(struct wdg_object));

   /* set the flags */
   (*wo)->flags = flags;
   (*wo)->type = type;
  
   /* call the specific function */
   switch (type) {
      case WDG_WINDOW:
         wdg_create_window(*wo);
         break;
         
      default:
         WDG_SAFE_FREE(*wo);
         return -WDG_EFATAL;
         break;
   }
   
   /* alloc the element in the object list */
   WDG_SAFE_CALLOC(wl, 1, sizeof(struct wdg_obj_list));

   /* link the object */
   wl->wo = *wo;

   /* insert it in the list */
   CIRCLEQ_INSERT_HEAD(&wdg_objects_list, wl, next);
   
   /* this is the root object */
   if (flags & WDG_OBJ_ROOT_OBJECT)
      wdg_root_obj = *wo;
   
   return WDG_ESUCCESS;
}

/*
 * destroy a wdg object by calling the callback function
 */
int wdg_destroy_object(struct wdg_object **wo)
{
   /* it was the root object ? */
   if ((*wo)->flags & WDG_OBJ_ROOT_OBJECT)
      wdg_root_obj = NULL;
  
   /* it was the focused one */
   if (wdg_focused_obj && wdg_focused_obj->wo == *wo)
      wdg_focused_obj = NULL;
   
   /* call the specialized destroy function */
   WDG_BUG_IF((*wo)->destroy == NULL);
   WDG_EXECUTE((*wo)->destroy, *wo);
   
   /* then free the object */
   WDG_SAFE_FREE(*wo);

   return WDG_ESUCCESS;
}

/*
 * set or reset the size of an object
 */
void wdg_resize_object(struct wdg_object *wo, int x1, int y1, int x2, int y2)
{
   /* set the new object cohordinates */
   wo->x1 = x1;
   wo->y1 = y1;
   wo->x2 = x2;
   wo->y2 = y2;

   /* call the specialized function */
   WDG_BUG_IF(wo->resize == NULL);
   WDG_EXECUTE(wo->resize, wo);
}

/*
 * display the object by calling the redraw function
 */
void wdg_draw_object(struct wdg_object *wo)
{
   /* display the object */
   WDG_BUG_IF(wo->redraw == NULL);
   WDG_EXECUTE(wo->redraw, wo);
}

/*
 * return the type of the object
 */
size_t wdg_get_type(struct wdg_object *wo)
{
   return wo->type;
}

/* EOF */

// vim:ts=3:expandtab

