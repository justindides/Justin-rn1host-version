/*
	PULUROBOT RN1-HOST Computer-on-RobotBoard main software

	(c) 2017-2018 Pulu Robotics and other contributors
	Maintainer: Antti Alhonen <antti.alhonen@iki.fi>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License version 2, as 
	published by the Free Software Foundation.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	GNU General Public License version 2 is supplied in file LICENSING.



*/

/*

	Currently, we recommend the following procedure to localize on existing maps:

	A) If possible, it's always most intuitive to map the new area by first time booting the robot
	in a logical position and angle: for example, (almost) mounted in the charger is a good place.
	If you do this to an accuracy of +/- 40cm and about +/- 4 degrees, you never need to do anything,
	localization succeeds to the existing map, since robot boots to the same zero coordinate with enough
	accuracy for the "normal" SLAM correction.


	B) If you want to localize to another place than the origin in an existing map, or to a more uncertain position,
	   please do the following:

	1) As the very first step, send TCP_CR_STATEVECT_MID: disable mapping_*, enable loca_*, so that the map isn't messed up
	   before succesful localization happens.

	2) If necessary, also set localize_with_big_search_area=1 or =2 in the statevect.

	3) Use TCP_CR_SETPOS_MID to send your estimate of the robot coordinates, with the following precision:
		+/- 4 degree angle,  +/-  400mm x&y, if localize_with_big_search_area state is 0 (normal operation)
		+/- 45 degree angle, +/- 2000mm x&y, if localize_with_big_search_area state is 1
		    Any angle,       +/- 2400mm x&y, if localize_with_big_search_area state is 2

	4) Instruct manual move(s) towards any desired direction where you can/want go to. If localize_with_big_search_area
	   state is set, more than normal number of lidar scans will be accumulated before the localization happens -
	   this typically means you need to move about 2-3 meters. Similarly, the localization with big search area
	   will take up to 20-30 seconds typically, or even minutes when set to 2.

	5) TCP_RC_LOCALIZATION_RESULT_MID is sent. If the localization results in high enough score,
	   localize_with_big_search_area is automatically unset. If the score is low, it remains set, until localization
	   is good. If you face a problem that you can't get high enough score, we advice you localize around a place with enough
	   visual clues, and make sure the map built earlier actually shows them (enough time to build a detailed, strong map).

	6) You can send TCP_CR_STATEVECT_MID with mapping_* turned on as well. These are not turned on automatically.



*/


//#define PULUTOF1_GIVE_RAWS

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/select.h>
#include <errno.h>
#include <math.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include "datatypes.h"
#include "hwdata.h"
#include "map_memdisk.h"
#include "mapping.h"
#include "uart.h"
#include "tcp_comm.h"
#include "tcp_parser.h"
#include "routing.h"
#include "utlist.h"

#include "pulutof.h"

#include "mcu_micronavi_docu.c"

#define DEFAULT_SPEEDLIM 45
#define MAX_CONFIGURABLE_SPEEDLIM 70

/*
	 Our task here is to restructure the existing rn1host code. The main thread should be divided in different 
 sub-threads : Mapping, Routing, Navigation and Communication. To reach this, we have first divided the main 
 thread in different functions, created from the existing code in the main thread. It was just a work of Copy-Paste. 
 Like this, the main thread is easier to read and to modify. These functions are made so they can be used as the 
 starting point of the new threads. The point is to always have a version that compiles. The final goal is to have 
 the same robot behaviour than before but with the possibility to modify and add new things to the code easier.
 The new threads starting points are route_fsm : navigation, routing_thread : routing, mapping_handling : mapping,
 communication_handling : communication. Routing_thread, mapping_handling and communication_handling are new functions.

Shared memory acces :
	 Right now on this version, the restructuration is done and it should compile but it surely won't work on the robot 
 since the different threads access to shared memory is not protected yet. There should be some conflict between the threads
 to read or wright variables at the same time. I believe the TCP/IP and UART buffers or the global variables such as world w
 could meet these problems. Mutex or semaphores can be used to protect memory areas. 

 	Right now, the navigation thread and routing thread wait for eachother to run, in a way that the navigation asks for a routing
 when it is needed. So there shouldn't be conflict between those. On the other hand, the mapping thread is running non stop in an
 infinite loop, so there could be conflicts between this one and the others. 
	
	 Concerning the communication thread, it concerns the incomming orders from the developer console commands or the user commands from
 the client. This thread shouldn't run continuously and wait for a command to come in to run it. But, when a command comes in concerning 
 the robot behaviour like its navigation, routing or mapping we should stop the concerned threads to run the command. The question is,
 should we cancel the threads concerned by a command or should we wait until the end of their loops. The answer concerns the priority 
 of the comming order, if it is a critical order like "Stop", or "go there manually", we should do this immediatly and so cancel the
 thread, run the comand (paramaters changes ect...), then recreate the thread and continue. If the order is not so important, we should 
 wait until the end of the concerned thread loop. Incoming message priority doesn't exist yet, but the refactoring has been done like
 if 3 priority bits existed just to give an idea of how the threads should be managed when a command comes in.
 
	 There are way more informations on this drive : https://docs.google.com/document/d/11pe03oysKdM3qin7-PSHSF_nNU7izzTMOuq2cZETWFc/edit#  
 I don't know if you can access it... Otherwise send me an email at justindides@hotmail.fr so I can give you the access to it or 
 to answer further questions.
*/


// Threading structure. It contains every mutex or condition that we use to manage the multithreading.



typedef struct
{
	pthread_t thread_navigation; 	// Threads declaration
	pthread_t thread_mapping; 
	pthread_t thread_routing;
 	pthread_t thread_communication;
	pthread_t thread_tof, thread_tof2;

	pthread_mutex_t mutex_token_routing;  // This mutex define what thread is running between the navigation and the routing. This can only work in our case where we only have one routing thread.

	pthread_cond_t cond_need_routing;     // When you need a routing, this condition becomes true.
	
	pthread_cond_t cond_continue_map, cond_continue_rout, cond_continue_nav;

	pthread_cond_t cond_routing_done, cond_mapping_done, cond_navigation_done; // These conditions are signaled at the end of the Threads loop.
	
	int dest_x, dest_y, dont_map_lidars, no_tight;
	int no_route_found;  // 1 = No route was found, 0 = A route has been found 

	int map_thread_cancel_state, rout_thread_cancel_state, nav_thread_cancel_state;	// Define if the threads can be canceled by pthread_cancel() : 1 = yes, 0 = no, can't be canceled at the moment.
	
	int map_thread_were_canceled, rout_thread_were_canceled, nav_thread_were_canceled;	// Defines if the thread has been canceled and should be recreated after running a command : 1 = canceled, 0 = not_canceled 

	int waiting_for_map_to_end, waiting_for_rout_to_end, waiting_for_nav_to_end;	// Flags put to 1 when we are waiting the end of a thread loop.

	pthread_mutex_t mutex_waiting_map_t, mutex_waiting_rout_t, mutex_waiting_nav_t;   // Those mutex are used when we are waiting a thread to end its loop. It is used with the condition waiting.

	int map_thread_on_pause, rout_thread_on_pause, nav_thread_on_pause;	// When we pause a thread to run a command, those flags are put to 1.
}
thread_struct;




/*  functions declaration */
int find_charger_procedure(thread_struct**);
void main_init(void);
void communication_handling(thread_struct*);  // Starting point of the communication thread
void cmd_from_developer_to_host(int, thread_struct**);
void cmd_from_client_to_host(int, thread_struct**);
void mapping_handling(thread_struct*);	// Starting point of the mapping thread
void tof_handling(void);
void lidar_handling(void);
void route_fsm(thread_struct*);	// Starting point of the navigation thread
void do_live_obstacle_checking(void);
void routing_thread(thread_struct*);	// Starting point of the routing thread
int run_search(int32_t, int32_t, int, int);
int rerun_search(void);
void send_route_end_status(uint8_t);
void thread_management_before_running_cmd(unsigned char, thread_struct**);
int thread_management_after_running_cmd(thread_struct**);



volatile int verbose_mode = 0;
volatile int send_raw_tof = -1;
volatile int send_pointcloud = 0; // 0 = off, 1 = relative to robot, 2 = relative to actual world coords

int max_speedlim = DEFAULT_SPEEDLIM;
int cur_speedlim = DEFAULT_SPEEDLIM;


state_vect_t state_vect =
{
	.v = {
	.loca_2d = 1,
	.loca_3d = 1,
	.mapping_2d = 1,
	.mapping_3d = 1,
	.mapping_collisions = 1,
	.keep_position = 1,
	.command_source = USER_IN_COMMAND,
	.localize_with_big_search_area = 0
	}
};

#define SPEED(x_) do{ cur_speedlim = ((x_)>max_speedlim)?(max_speedlim):(x_); } while(0);

double subsec_timestamp()
{
	struct timespec spec;
	clock_gettime(CLOCK_MONOTONIC, &spec);

	return (double)spec.tv_sec + (double)spec.tv_nsec/1.0e9;
}

int live_obstacle_checking_on = 1; // only temporarily disabled by charger mounting code.
int pos_corr_id = 42;
#define INCR_POS_CORR_ID() {pos_corr_id++; if(pos_corr_id > 99) pos_corr_id = 0;}


int map_significance_mode = MAP_SEMISIGNIFICANT_IMGS | MAP_SIGNIFICANT_IMGS;

uint32_t robot_id = 0xacdcabba; // Hopefully unique identifier for the robot.

int cmd_state;

extern world_t world;
#define BUFLEN 2048

int32_t cur_ang, cur_x, cur_y;
double robot_pos_timestamp;
int32_t cur_compass_ang;
int compass_round_active;

typedef struct
{
	int x;
	int y;
	int backmode;
	int take_next_early;
	int timeout;
} route_point_t;

#define THE_ROUTE_MAX 200
route_point_t the_route[THE_ROUTE_MAX];
int the_route_len = 0;

int do_follow_route = 0;
int route_finished_or_notfound = 0;
int lookaround_creep_reroute = 0;
int route_pos = 0;
int start_route = 0;
int id_cnt = 1;
int good_time_for_lidar_mapping = 0;
static int maneuver_cnt = 0; // to prevent too many successive maneuver operations. Used in do_live_obstacle_checking

#define sq(x) ((x)*(x))

#define NUM_LATEST_LIDARS_FOR_ROUTING_START 4
lidar_scan_t* lidars_to_map_at_routing_start[NUM_LATEST_LIDARS_FOR_ROUTING_START];

/**
* Calls the function wich send the tcp/ip state info 
*
* Input parameters:
* info_state_t state : State of the tcp/ip communication (ex :"Think", "Iddle", "Daiju_mode")
* Input memory areas:
* None
* Output memory areas:
* None
* Return value:
* None
*/
void send_info(info_state_t state)
{
	if(tcp_client_sock >= 0) tcp_send_info_state(state);
}


int32_t charger_ang;
int charger_fwd;
int charger_first_x, charger_first_y, charger_second_x, charger_second_y;
#define CHARGER_FIRST_DIST 1000
#define CHARGER_SECOND_DIST 500
#define CHARGER_THIRD_DIST  170

void save_robot_pos()
{
	FILE* f_cha = fopen("/home/hrst/rn1-host/robot_pos.txt", "w");
	if(f_cha)
	{
		fprintf(f_cha, "%d %d %d\n", cur_ang, cur_x, cur_y);
		fclose(f_cha);
	}
}

void retrieve_robot_pos()
{
	int32_t ang;
	int x; int y;
	FILE* f_cha = fopen("/home/hrst/rn1-host/robot_pos.txt", "r");
	if(f_cha)
	{
		fscanf(f_cha, "%d %d %d", &ang, &x, &y);
		fclose(f_cha);
		set_robot_pos(ang, x, y);
	}
}

void conf_charger_pos()  // call when the robot is *in* the charger.
{
	int32_t da, dx, dy;
	map_lidars(&world, NUM_LATEST_LIDARS_FOR_ROUTING_START, lidars_to_map_at_routing_start, &da, &dx, &dy);
	INCR_POS_CORR_ID();

	int32_t cha_ang = cur_ang-da; int cha_x = cur_x+dx; int cha_y = cur_y+dy;

	correct_robot_pos(da, dx, dy, pos_corr_id);

	printf("Set charger pos at ang=%d, x=%d, y=%d\n", cha_ang, cha_x, cha_y);
	charger_first_x = (float)cha_x - cos(ANG32TORAD(cha_ang))*(float)CHARGER_FIRST_DIST;
	charger_first_y = (float)cha_y - sin(ANG32TORAD(cha_ang))*(float)CHARGER_FIRST_DIST;	
	charger_second_x = (float)cha_x - cos(ANG32TORAD(cha_ang))*(float)CHARGER_SECOND_DIST;
	charger_second_y = (float)cha_y - sin(ANG32TORAD(cha_ang))*(float)CHARGER_SECOND_DIST;
	charger_fwd = CHARGER_SECOND_DIST-CHARGER_THIRD_DIST;
	charger_ang = cha_ang;

	FILE* f_cha = fopen("/home/hrst/rn1-host/charger_pos.txt", "w");
	if(f_cha)
	{
		fprintf(f_cha, "%d %d %d %d %d %d\n", charger_first_x, charger_first_y, charger_second_x, charger_second_y, charger_ang, charger_fwd);
		fclose(f_cha);
	}
}

void read_charger_pos()
{
	FILE* f_cha = fopen("/home/hrst/rn1-host/charger_pos.txt", "r");
	if(f_cha)
	{
		fscanf(f_cha, "%d %d %d %d %d %d", &charger_first_x, &charger_first_y, &charger_second_x, &charger_second_y, &charger_ang, &charger_fwd);
		fclose(f_cha);
		printf("charger position retrieved from file: %d, %d --> %d, %d, ang=%d, fwd=%d\n", charger_first_x, charger_first_y, charger_second_x, charger_second_y, charger_ang, charger_fwd);
	}
}


void save_pointcloud(int n_points, xyz_t* cloud)
{
	static int pc_cnt = 0;
	char fname[256];
	snprintf(fname, 255, "cloud%05d.xyz", pc_cnt);
	printf("Saving pointcloud with %d samples to file %s.\n", n_points, fname);
	FILE* pc_csv = fopen(fname, "w");
	if(!pc_csv)
	{
		printf("Error opening file for write.\n");
	}
	else
	{
		for(int i=0; i < n_points; i++)
		{
			fprintf(pc_csv, "%d %d %d\n",cloud[i].x, -1*cloud[i].y, cloud[i].z);
		}
		fclose(pc_csv);
	}

	pc_cnt++;
	if(pc_cnt > 99999) pc_cnt = 0;
}



int cal_x_d_offset = 0;
int cal_y_d_offset = 0;
float cal_x_offset = 40.0;
float cal_y_offset = 0.0;
float cal_x_sin_mult = 1.125;
float cal_y_sin_mult = 1.125;

#ifdef PULUTOF1
void request_tof_quit(void);
#endif

volatile int retval = 0;
int flush_3dtof = 0;   // Put in global because of the main division. 
int lidar_ignore_over = 0;   // Put in global because of the main division.
int find_charger_state = 0;   // To complicated to use it with pointers, way easier in global. This is the finding charger procedure state. 0 = Not looking for the charger at the moment.

#ifdef PULUTOF1
void* start_tof(void*);
#endif

int main(int argc, char** argv)
{

	int ret;

	thread_struct host_t = 
	{
		.mutex_token_routing = PTHREAD_MUTEX_INITIALIZER,
		.cond_need_routing = PTHREAD_COND_INITIALIZER,
		.cond_continue_map = PTHREAD_COND_INITIALIZER, .cond_continue_rout = PTHREAD_COND_INITIALIZER, .cond_continue_nav = PTHREAD_COND_INITIALIZER,
		.cond_routing_done = PTHREAD_COND_INITIALIZER, .cond_mapping_done = PTHREAD_COND_INITIALIZER, .cond_navigation_done = PTHREAD_COND_INITIALIZER,
		.dest_x = 0, .dest_y = 0, .dont_map_lidars = 0, .no_tight= 1,
		.no_route_found = 1,  // 1 = No route was found, 0 = A route has been found 


		.map_thread_cancel_state = 1, .rout_thread_cancel_state = 1, .nav_thread_cancel_state = 1,	// Define if the threads can be canceled by pthread_cancel() : 1 = yes, 0 = no, can't
														// be canceled at the moment.
	
		.map_thread_were_canceled = 0, .rout_thread_were_canceled = 0, .nav_thread_were_canceled = 0,	// Defines if the thread has been canceled and should be recreated after running a 
														// command : 1 = canceled, 0 = not_canceled 

		.waiting_for_map_to_end = 0, .waiting_for_rout_to_end = 0, .waiting_for_nav_to_end = 0,	// Flags put to 1 when we are waiting the end of a thread loop.	

		.mutex_waiting_map_t = PTHREAD_MUTEX_INITIALIZER, .mutex_waiting_rout_t = PTHREAD_MUTEX_INITIALIZER, .mutex_waiting_nav_t = PTHREAD_MUTEX_INITIALIZER,   

		.map_thread_on_pause = 0, .rout_thread_on_pause = 0, .nav_thread_on_pause = 0,	// When we pause a thread to run a command, those flags are put to 1.

	};   

	
	main_init();	// Init variables and communication
	

	//communication_handling(&host_t);

	// Communication thread : Handle communications CLIENT/SERVER <-> HOST and devs comamnds from standard input. Init the UART as well.
	if( (ret = pthread_create(&host_t.thread_communication, NULL, communication_handling, &host_t )) !=0 )
	{
		printf("ERROR: communication thread creation failed, ret = %d\n", ret);
		return -1;
	}
		
	// Mapping Thread 
	if( (ret = pthread_create(&host_t.thread_mapping, NULL, mapping_handling, &host_t)) != 0) 
	{
		printf("ERROR: maping thread creation failed, ret = %d\n", ret);
		return -1;
	}		

	// Navigation Thread
	if( (ret = pthread_create(&host_t.thread_navigation, NULL, route_fsm, &host_t)) != 0) 
	{
		printf("ERROR: navigation thread creation failed, ret = %d\n", ret);
		return -1;
	}

	// Routing thread
	if( (ret = pthread_create(&host_t.thread_routing, NULL, routing_thread, &host_t)) != 0) 
	{
		printf("ERROR: routing thread creation failed, ret = %d\n", ret);
		return -1;
	}		
	   
	//mapping_handling();



#ifdef PULUTOF1
	if( (ret = pthread_create(&host_t.thread_tof, NULL, pulutof_poll_thread, NULL)) )
	{
		printf("ERROR: tof3d access thread creation, ret = %d\n", ret);
		return -1;
	}

	#ifndef PULUTOF1_GIVE_RAWS
		if( (ret = pthread_create(&host_t.thread_tof2, NULL, pulutof_processing_thread, NULL)) )
		{
			printf("ERROR: tof3d processing thread creation, ret = %d\n", ret);
			return -1;
		}
	#endif
#endif

	pthread_join(host_t.thread_communication, NULL); // This thread never end so this should block the main().

#ifdef PULUTOF1
	pthread_join(thread_tof, NULL);
	pthread_join(thread_tof2, NULL);
#endif

#ifdef PULUTOF1		// This shouldn t be there
	request_tof_quit();
#endif

	return retval;
}



// New and old functions : They are made to simplify the main thread, taking parts of the main thread code and putting it into these functions. This doesn´t shorten the code but make the main thread more readable and easier to modify.  
//  These new functions are also he future starting point of the new threads. 
//**************************************************************************************************************************************************************************************************************************************************************************


// Just some init before entering the while(1) loop.
void main_init(void)
{
	if(init_uart())
	{
		fprintf(stderr, "uart initialization failed.\n");
		return NULL;
	}

	if(init_tcp_comm())
	{
		fprintf(stderr, "TCP communication initialization failed.\n");
		return NULL;
	}

	srand(time(NULL));

	send_keepalive();
	daiju_mode(0);
	correct_robot_pos(0,0,0, pos_corr_id); // To set the pos_corr_id.
	turn_and_go_rel_rel(-5*ANG_1_DEG, 0, 25, 1);
	sleep(1);
	send_keepalive();
	turn_and_go_rel_rel(10*ANG_1_DEG, 0, 25, 1);
	sleep(1);
	send_keepalive();
	turn_and_go_rel_rel(-5*ANG_1_DEG, 50, 25, 1);
	sleep(1);
	send_keepalive();
	turn_and_go_rel_rel(0, -50, 25, 1);
	sleep(1);

	set_hw_obstacle_avoidance_margin(0);
	
	return;
}


/***********************************************************************************************************************************************************************************************************************************************************************/
/***********************************************************************************************************************************************************************************************************************************************************************/

					// NAVIGATION THREAD

/*
*
* Here we have the following route function. It describe the protocol to follow when the robot has a route or not. This protocol consists in looking if it is possible to follow the route, if it´s not the robot will creep a little bit arround him to find another way to get to the route. If there´s still no way to follow the route, it will try to reroute. If the 
rerouting fails (no route found), the robot will go daiju mode to find a position where you can reroute. To do this, it uses the movement (hwdata) and routing functions such as : move_to(), turn_abs_and_go_rel(), line_of_sight(), check_drect_route, test_robot_turning...  "Micronavi" variables are informations comming directly from the microcontroller (Brain). This allows a direct  
*
* Input parameters:
* p_host_t : Pointer toward the general treading structure.
*
* Input memory areas:
* static int micronavi_stops; static double timestamp;double stamp; static int creep_cnt; int creep amount; int dx/y;int ang; int dest_x/y;int id; int prev_incr
*
* Output memory areas:
* lookaround_creep_reroute : This describe at wich "step" of the protocol we are in. Goes from 0(no protocol) to step 12; do_follow_route; route_finished_or_notfound; route_pos;
start_route; id_cnt;good_time_for_lidar_mapping;cur_x/y; the_route[route_pos].x/y; cur_xy_move.micronavi/feedback_stop_flags; cur_xy_move.remaining
*
* Return value:
*  none (void)
*/

void route_fsm(thread_struct* p_host_t)
{
	static int micronavi_stops = 0;
	static double timestamp;
	static int creep_cnt;
	int reret; // Rerouting variable, typical value 0. Takes another value if rerouting is needed (not handled yet).

	while(1)
	{	

		pthread_mutex_lock (&p_host_t->mutex_token_routing);

		if(lookaround_creep_reroute)
		{
			if(check_direct_route_non_turning_mm(cur_x, cur_y, the_route[route_pos].x, the_route[route_pos].y))
			{// If there's a direct route without the need to turn, use it
				printf("Direct line-of-sight has appeared to the next waypoint, resuming following the route.\n");
				lookaround_creep_reroute = 0;
				do_follow_route = 1;
				id_cnt++; if(id_cnt > 7) id_cnt = 1;
				send_info(the_route[route_pos].backmode?INFO_STATE_REV:INFO_STATE_FWD);
				move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4) | ((route_pos)&0b1111), cur_speedlim, 0);
			}
		}

		const float lookaround_turn = 10.0;

		// Those 6 next step are made to check if there are any obstacles around the route toward the destination. If possible, the robot will turn toward
		// the right of the destination and then to the left to cover the interval [destination-1.8*lookaround_turn;destination+1.8*lookaround_turn]. 
		// When this checking is done, it will turn toward the destination.
		 
	
		if(lookaround_creep_reroute == 1)   // The robot backs off 50mm to reroute
		{
			do_follow_route = 0;
			start_route = 0;

			printf("Lookaround, creep & reroute procedure started; backing off 50 mm.\n");
			turn_and_go_abs_rel(cur_ang, -50, 13, 1);
			timestamp = subsec_timestamp();
			lookaround_creep_reroute++;  //Go to the next step
		}
		else if(lookaround_creep_reroute == 2)
		{
			double stamp;
			if( (stamp=subsec_timestamp()) > timestamp+1.0)
			{
				if(doing_autonomous_things()) // The robot is mapping, we'll research for a route
				{
					printf("Robot is mapping autonomously: no need to clear the exact route right now, skipping lookaround & creep\n");
					
					pthread_cond_signal(&p_host_t->cond_need_routing);	// Asking for a routing to the host_t.dest_x/y
					pthread_cond_wait(&p_host_t->cond_routing_done, &p_host_t->mutex_token_routing);

					lookaround_creep_reroute = 0;
				}
				else
				{
					int dx = the_route[route_pos].x - cur_x; //Relatives X and Y of the destination. Then calculate the angle
					int dy = the_route[route_pos].y - cur_y;
					float ang = atan2(dy, dx) /*<- ang to dest*/ - DEGTORAD(lookaround_turn); // First angle is destination-lookaround_turn

					if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))	// If it is possible to turn toward this angle, does it.
					{
						//printf("Can turn to %.1f deg, doing it.\n", -1*lookaround_turn);
						turn_and_go_abs_rel(RADTOANG32(ang), 0, 13, 1);  // Turn toward the angle
					}
					else   // If it is not possible,
					{
						//printf("Can't turn to %.1f deg, wiggling a bit.\n", -1*lookaround_turn);
						turn_and_go_abs_rel(cur_ang-4*ANG_1_DEG, 0, 13, 1); // If this angle is not reachable, just turn 4° from current location
					}
					timestamp = subsec_timestamp();
					lookaround_creep_reroute++;  // goes to the next step
				}
			}
		}
		else if(lookaround_creep_reroute == 3)
		{
			double stamp;
			if( (stamp=subsec_timestamp()) > timestamp+1.0)
			{
				int dx = the_route[route_pos].x - cur_x;
				int dy = the_route[route_pos].y - cur_y;
				float ang = atan2(dy, dx) /*<- ang to dest*/ - DEGTORAD(1.8*lookaround_turn);

				if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
				{
					//printf("Can turn to %.1f deg, doing it.\n", -1.8*lookaround_turn);
					turn_and_go_abs_rel(RADTOANG32(ang), -20, 13, 1);
				}
				else
				{
					//printf("Can't turn to %.1f deg, wiggling a bit.\n", -1.8*lookaround_turn);
					turn_and_go_abs_rel(cur_ang-4*ANG_1_DEG, 0, 13, 1);
				}
				timestamp = subsec_timestamp();
				lookaround_creep_reroute++;
			}
		}
		else if(lookaround_creep_reroute == 4)
		{
			double stamp;
			if( (stamp=subsec_timestamp()) > timestamp+1.0)
			{
				int dx = the_route[route_pos].x - cur_x;
				int dy = the_route[route_pos].y - cur_y;
				float ang = atan2(dy, dx) /*<- ang to dest*/ + DEGTORAD(lookaround_turn);

				if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
				{
					//printf("Can turn to %.1f deg, doing it.\n",lookaround_turn);
					turn_and_go_abs_rel(RADTOANG32(ang), 0, 13, 1);
				}
				else
				{
					//printf("Can't turn to %.1f deg, wiggling a bit.\n",lookaround_turn);
					turn_and_go_abs_rel(cur_ang+12*ANG_1_DEG, 0, 13, 1);
				}
				timestamp = subsec_timestamp();
				lookaround_creep_reroute++;
			}
		}
		else if(lookaround_creep_reroute == 5)
		{
			double stamp;
			if( (stamp=subsec_timestamp()) > timestamp+1.0)
			{
				int dx = the_route[route_pos].x - cur_x;
				int dy = the_route[route_pos].y - cur_y;
				float ang = atan2(dy, dx) /*<- ang to dest*/ + DEGTORAD(1.8*lookaround_turn);

				if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))
				{
					//printf("Can turn to %.1f deg, doing it.\n", 1.8*lookaround_turn);
					turn_and_go_abs_rel(RADTOANG32(ang), 0, 13, 1);
				}
				else
				{
					//printf("Can't turn to %.1f deg, wiggling a bit.\n", 1.8*lookaround_turn);
					turn_and_go_abs_rel(cur_ang+4*ANG_1_DEG, 0, 13, 1);
				}
				timestamp = subsec_timestamp();
				lookaround_creep_reroute++;
			}
		}
		else if(lookaround_creep_reroute == 6)
		{
			creep_cnt = 0;
			double stamp;
			if( (stamp=subsec_timestamp()) > timestamp+1.0)
			{
				int dx = the_route[route_pos].x - cur_x;
				int dy = the_route[route_pos].y - cur_y;
				float ang = atan2(dy, dx) /*<- ang to dest*/;

				if(test_robot_turn_mm(cur_x, cur_y, ANG32TORAD(cur_ang),  ang))   // After those last checkings, if it is possible turn toward the destination angle 
				{																	// Then, go step 7.
					//printf("Can turn towards the dest, doing it.\n");
					turn_and_go_abs_rel(RADTOANG32(ang), 50, 13, 1);
				}
				else		// If the destination angle is not reachable, we'll have to reroute. If the reroute fails => Daiju mode on, then get to step 8
				{
					printf("Can't turn towards the dest, rerouting.\n");
						
					pthread_cond_signal(&p_host_t->cond_need_routing);	// Asking for a routing to the host_t.dest_x/y
					pthread_cond_wait(&p_host_t->cond_routing_done, &p_host_t->mutex_token_routing);

					if(p_host_t->no_route_found == 1)
					{
						printf("Routing failed in start, going to daiju mode for a while.\n");
						send_info(INFO_STATE_DAIJUING);
						daiju_mode(1);
						lookaround_creep_reroute = 8;
					}
					else	// If an other route is found, we remake the same checking procedure since step 1 with the new destination.
					{
						printf("Routing succeeded, or failed later. Stopping lookaround, creep & reroute procedure.\n");
						lookaround_creep_reroute = 0;  
					}
				}
				timestamp = subsec_timestamp();
				lookaround_creep_reroute++;
			}
		}
		else if(lookaround_creep_reroute == 7)	// Step 7
		{
			static double time_interval = 2.5;
			double stamp;
			if( (stamp=subsec_timestamp()) > timestamp+time_interval)
			{
				int dx = the_route[route_pos].x - cur_x;
				int dy = the_route[route_pos].y - cur_y;
				int dist = sqrt(sq(dx)+sq(dy));	// Calculate the distance between the robot and the destination from the desination
				if(dist > 300 && creep_cnt < 3)
				{
					float ang = atan2(dy, dx) /*<- ang to dest*/;
					int creep_amount = 100;
					int dest_x = cur_x + cos(ang)*creep_amount;
					int dest_y = cur_y + sin(ang)*creep_amount;
					int hitcnt = check_direct_route_non_turning_hitcnt_mm(cur_x, cur_y, dest_x, dest_y); // Check how many obastacles there are towards the destination
					if(hitcnt < 1)  // If the line of sight is clear, go 10cm forward to creep again
					{
						//printf("Can creep %d mm towards the next waypoint, doing it\n", creep_amount);
						time_interval = 2.5;
						turn_and_go_abs_rel(RADTOANG32(ang) + ((creep_cnt&1)?(5*ANG_1_DEG):(-5*ANG_1_DEG)), creep_amount, 15, 1);
					}
					else  // If there is no line of sight, we'll stop creeping 
					{
						creep_cnt=99;
					}
					creep_cnt++;
				}
				else // If we have to stop creeping or we have been creeping for too long (creep_cnt > 3) then, we'll search for another route
				{
					printf("We have creeped enough (dist to waypoint=%d, creep_cnt=%d), no line of sight to the waypoint, trying to reroute\n",
						dist, creep_cnt);
					
					pthread_cond_signal(&p_host_t->cond_need_routing);	// Asking for a routing to the host_t.dest_x/y
					pthread_cond_wait(&p_host_t->cond_routing_done, &p_host_t->mutex_token_routing);

					if(p_host_t->no_route_found == 1)	// If we can't find a route, go daiju mode and move to step 8
					{
						printf("Routing failed in start, going to daiju mode for a while.\n");
						daiju_mode(1);
						send_info(INFO_STATE_DAIJUING);
						lookaround_creep_reroute++;
					}
					else	// If a route has been found, stop the procedure.
					{
						printf("Routing succeeded, or failed later. Stopping lookaround, creep & reroute procedure.\n");
						if(reret != 0) send_route_end_status(reret);
						lookaround_creep_reroute = 0;
					}

				}
				timestamp = subsec_timestamp();
			}
		}
		else if(lookaround_creep_reroute >= 8 && lookaround_creep_reroute < 12)	// Here we'll keep looking for a route in Daiju mode. If a route is found,
		{									// stop the procedure. If not, we keep daijuing for some time. If we still
			double stamp;							// can't find nothing, move to step 12.
			if( (stamp=subsec_timestamp()) > timestamp+5.0)
			{
				printf("Daijued enough.\n");
				daiju_mode(0);
				
				pthread_cond_signal(&p_host_t->cond_need_routing);	// Asking for a routing to the host_t.dest_x/y
				pthread_cond_wait(&p_host_t->cond_routing_done, &p_host_t->mutex_token_routing);

				if(p_host_t->no_route_found == 1)
				{
					printf("Routing failed in start, going to daiju mode for a bit more...\n");
					daiju_mode(1);
					send_info(INFO_STATE_DAIJUING);
					lookaround_creep_reroute++;
					timestamp = subsec_timestamp();
				}
				else
				{
					printf("Routing succeeded, or failed later. Stopping lookaround, creep & reroute procedure.\n");
					if(reret != 0) send_route_end_status(reret);
					lookaround_creep_reroute = 0;
				}

			}
		}
		else if(lookaround_creep_reroute == 12)	// We haven't found a route, stop the look qround creep process ?
		{
			printf("Giving up lookaround, creep & reroute procedure!\n");
			lookaround_creep_reroute = 0;
		}

		if(start_route)	// If he has found a route during the process, goes toward it ?
		{
			printf("Start going id=%d!\n", id_cnt<<4);
			move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4), cur_speedlim, 0);
			send_info(the_route[route_pos].backmode?INFO_STATE_REV:INFO_STATE_FWD);

			start_route = 0;
		}

		if(do_follow_route)  	// This part will start the previous procedure if there is a route to follow and the ids correspond ? (What IDS ?) 
		{
			int id = cur_xymove.id;

			if(((id&0b1110000) == (id_cnt<<4)) && ((id&0b1111) == ((route_pos)&0b1111)))
			{
				if(cur_xymove.micronavi_stop_flags || cur_xymove.feedback_stop_flags) 
				{
					if(micronavi_stops < 7)
					{
						printf("Micronavi STOP, entering lookaround_creep_reroute\n");  // That will remake the procedure until the conditions are false
						micronavi_stops++;
						lookaround_creep_reroute = 1;
					}
					else
					{
						printf("Micronavi STOP, too many of them already, rerouting.\n");  // After too many procedure, it will reroute.
						
						pthread_cond_signal(&p_host_t->cond_need_routing);	// Asking for a routing to the host_t.dest_x/y
						pthread_cond_wait(&p_host_t->cond_routing_done, &p_host_t->mutex_token_routing);

						if(p_host_t->no_route_found == 1)
						{
							printf("Routing failed in start, todo: handle this situation.\n");
						}
						else
						{
							printf("Routing succeeded, or failed later.\n");
						}
					}
				}
				else if(id_cnt == 0) // Zero id move is a special move during route following
				{
					if(cur_xymove.remaining < 30)  // If the next point to reach on the route has a direct line of sight, we´ĺl skip it and pass to the nexx 
					{				// Point
						while(the_route[route_pos].backmode == 0 && route_pos < the_route_len-1)
						{
							if( (sq(cur_x-the_route[route_pos+1].x)+sq(cur_y-the_route[route_pos+1].y) < sq(800) )
							    && check_direct_route_mm(cur_ang, cur_x, cur_y, the_route[route_pos+1].x, the_route[route_pos+1].y))
							{
								printf("Maneuver done; skipping point (%d, %d), going directly to (%d, %d)\n", the_route[route_pos].x,
								       the_route[route_pos].y, the_route[route_pos+1].x, the_route[route_pos+1].y);
								route_pos++;
							}
							else
							{
								break;
							}
						}
						id_cnt = 1;
						printf("Maneuver done, redo the waypoint, id=%d!\n", (id_cnt<<4) | ((route_pos)&0b1111));
						move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4) | ((route_pos)&0b1111), cur_speedlim, 0);
						send_info(the_route[route_pos].backmode?INFO_STATE_REV:INFO_STATE_FWD);

					}
				}
				else
				{
					if(cur_xymove.remaining < 250)
					{
						good_time_for_lidar_mapping = 1;
					}

					if(cur_xymove.remaining < the_route[route_pos].take_next_early)
					{
						maneuver_cnt = 0;
						if(route_pos < the_route_len-1)
						{
							route_pos++;

							// Check if we can skip some points:
							while(the_route[route_pos].backmode == 0 && route_pos < the_route_len-1)
							{
								if( (sq(cur_x-the_route[route_pos+1].x)+sq(cur_y-the_route[route_pos+1].y) < sq(800) )
								  && check_direct_route_mm(cur_ang, cur_x, cur_y, the_route[route_pos+1].x, the_route[route_pos+1].y))
								{
									printf("skipping point (%d, %d), going directly to (%d, %d)\n", the_route[route_pos].x,
									       the_route[route_pos].y, the_route[route_pos+1].x, the_route[route_pos+1].y);
									route_pos++;
								}
								else
								{
									break;
								}
							}
							printf("Take the next, id=%d!\n", (id_cnt<<4) | ((route_pos)&0b1111));
							move_to(the_route[route_pos].x, the_route[route_pos].y, the_route[route_pos].backmode, (id_cnt<<4) | ((route_pos)&0b1111), cur_speedlim, 0);
							send_info(the_route[route_pos].backmode?INFO_STATE_REV:INFO_STATE_FWD);
							micronavi_stops = 0;
						}
						else
						{
							printf("Done following the route.\n");
							send_info(INFO_STATE_IDLE);
							micronavi_stops = 0;
							do_follow_route = 0;
							route_finished_or_notfound = 1;
							send_route_end_status(TCP_RC_ROUTE_STATUS_SUCCESS);
						}
					}
					else if(live_obstacle_checking_on)
					{
						// Check if obstacles have appeared in the map.

						static double prev_incr = 0.0;
						double stamp;
						if( (stamp=subsec_timestamp()) > prev_incr+0.10)
						{
							prev_incr = stamp;

							if(robot_pos_timestamp < stamp-0.20)
							{
								//printf("Skipping live obstacle checking due to stale robot pos.\n");
							}
							else
							{
								do_live_obstacle_checking();
							}
						}
					}
				}
			}

		}

		
		if(find_charger_state != 0)	// Charger finding procedure. If charger_find_state is at 0, then we are not looking for the charger, no need for the procedure.
		{
			int ret = find_charger_procedure(&p_host_t);
			if(ret == 0)
			{
				printf("No route found to the charger, the procedure stops (Step : %d). \n", find_charger_state);
			}
			else if(ret == 1)
			{
				printf("The charger finding procedure is runing and is at Step : %d. \n", find_charger_state);

			}
			else if(ret == 2)
			{
				printf("Success, we are at the charger, the procedure stop (Step : %d). \n", find_charger_state);

			}
		}
		pthread_mutex_unlock (&p_host_t->mutex_token_routing);

		if(p_host_t->waiting_for_nav_to_end)	// If a command is waiting to be executed after the end of this loop. Wroks with the thread_management.. functions
		{
			p_host_t->nav_thread_on_pause = 1;	// The thread has to be on pause while we run teh command.
	
			pthread_mutex_lock(&p_host_t->mutex_waiting_nav_t);				
	
			while(p_host_t->waiting_for_nav_to_end)
			{
				pthread_cond_signal(&p_host_t->cond_navigation_done);	// First, we signal the loop has ended
			}
			pthread_mutex_unlock(&p_host_t->mutex_waiting_nav_t);

			sleep(1);
			
			pthread_mutex_lock(&p_host_t->mutex_waiting_nav_t);		
			pthread_cond_wait(&p_host_t->cond_continue_nav, &p_host_t->mutex_waiting_nav_t);	// We wait for the command to be done. 
			pthread_mutex_unlock(&p_host_t->mutex_waiting_nav_t);	
		}


	} // End of while(1) loop

}


/* Taking the charger finfing procedure from the main thread into this function. This way, all this procedure can be called through this function.
This procedure is described in 8 Step (find_charger_state). 0 means you are not looking for the charger, 8 means you are charging.
 */

int find_charger_procedure(thread_struct **pp_host_t)
{
	double chafind_timestamp = 0.0;

	if(find_charger_state < 4)
		live_obstacle_checking_on = 1;
	else
		live_obstacle_checking_on = 0;

	if(find_charger_state == 1)	// Starting the procedure to find the charger 
	{
		state_vect.v.keep_position = 1;
		daiju_mode(0);
		
		(**pp_host_t).dest_x = charger_first_x;
		(**pp_host_t).dest_y = charger_first_y;
		(**pp_host_t).dont_map_lidars = 0;
		(**pp_host_t).no_tight = 1;

		pthread_cond_signal(&(**pp_host_t).cond_need_routing);	// Asking for a routing to the host_t.dest_x/y
		pthread_cond_wait(&(**pp_host_t).cond_routing_done, &(**pp_host_t).mutex_token_routing);

		if((**pp_host_t).no_route_found == 1)
		{
			printf("Finding charger (first point) failed.\n");
			find_charger_state = 0;
			return 0;
		}
		else
			find_charger_state += 1;
	}
	else if(find_charger_state == 2)
	{
		if(!do_follow_route && !lookaround_creep_reroute)
		{
			if(sq(cur_x-charger_first_x) + sq(cur_y-charger_first_y) > sq(300))
			{
				printf("We are not at the first charger point, trying again.\n");
				find_charger_state = 1;
			}
			else
			{
				send_info(INFO_STATE_THINK);
				printf("At first charger point, turning for charger.\n");
				turn_and_go_abs_rel(charger_ang, 0, 23, 1);
				find_charger_state += 1;
				chafind_timestamp = subsec_timestamp();

			}
		}
	}
	else if(find_charger_state == 3)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > chafind_timestamp+2.5)
		{
			send_info(INFO_STATE_THINK);

			chafind_timestamp = stamp;

			printf("Turned at first charger point, mapping lidars for exact pos.\n");

			int32_t da, dx, dy;
			map_lidars(&world, NUM_LATEST_LIDARS_FOR_ROUTING_START, lidars_to_map_at_routing_start, &da, &dx, &dy);
			INCR_POS_CORR_ID();
			correct_robot_pos(da, dx, dy, pos_corr_id);
			lidar_ignore_over = 0;
			find_charger_state += 1;
		}
	}
	else if(find_charger_state == 4)
	{
		if(lidar_ignore_over && subsec_timestamp() > chafind_timestamp+3.0)
		{
			printf("Going to second charger point.\n");
			send_info(INFO_STATE_FWD);
			move_to(charger_second_x, charger_second_y, 0, 0x7f, 20, 1);
			find_charger_state += 1;
		}
	}
	else if(find_charger_state == 5)
	{
		if(cur_xymove.id == 0x7f && cur_xymove.remaining < 10)
		{
			if(sq(cur_x-charger_second_x) + sq(cur_y-charger_second_y) > sq(180))
			{
				printf("We are not at the second charger point, trying again.\n");
				find_charger_state = 1;
			}
			else
			{
				send_info(INFO_STATE_THINK);

				turn_and_go_abs_rel(charger_ang, charger_fwd, 20, 1);
				chafind_timestamp = subsec_timestamp();
				find_charger_state += 1;
			}
		}
	}
	else if(find_charger_state == 6)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > chafind_timestamp+3.0)
		{
			send_info(INFO_STATE_THINK);
			chafind_timestamp = stamp;
			turn_and_go_abs_rel(charger_ang, 0, 23, 1);
			find_charger_state += 1;
		}
	}
	else if(find_charger_state == 7)
	{
		double stamp;
		if( (stamp=subsec_timestamp()) > chafind_timestamp+1.5)
		{
			chafind_timestamp = stamp;
			send_info(INFO_STATE_THINK);
			printf("Requesting charger mount.\n");
			hw_find_charger();
			find_charger_state += 1;
		}
	}
	else if(find_charger_state == 8)
	{
		if(!pwr_status.charging && !pwr_status.charged)
		{
			if(subsec_timestamp() > chafind_timestamp+90.0)
			{
				printf("WARNING: Not charging (charger mount failure?). Retrying driving to charger.\n");
				find_charger_state = 1;
			}
		}
		else
		{
			send_info(INFO_STATE_CHARGING);
			find_charger_state = 0;
			printf("Robot charging succesfully.\n");
			return 2;
		}
	}
	return 1;
}


void do_live_obstacle_checking()
{
	if(the_route[route_pos].backmode == 0)
	{
		int32_t target_x, target_y;
		int dx = the_route[route_pos].x - cur_x;
		int dy = the_route[route_pos].y - cur_y;

		int32_t dist_to_next = sqrt(sq(dx)+sq(dy));

		// Obstacle avoidance looks towards the next waypoint; if more than max_dist_to_next away,
		// it looks towards the staight line from cur_pos to the next waypoint, but only for the lenght of max_dist_to_next.
		// If there are obstacles on the straight line-of-sight, extra waypoint is searched for so that the number of
		// obstacles are minimized.
		// so target_x, target_y is only the target which is looked at. It's never a move_to target directly, the next waypoint is.

		const int32_t max_dist_to_next = 1200;
		if(dist_to_next < max_dist_to_next)
		{
			target_x = the_route[route_pos].x;
			target_y = the_route[route_pos].y;
		}
		else
		{
			float ang_to_target = atan2(dy, dx);
			target_x = cur_x + max_dist_to_next*cos(ang_to_target);
			target_y = cur_y + max_dist_to_next*sin(ang_to_target);
		}

		int hitcnt = check_direct_route_non_turning_hitcnt_mm(cur_x, cur_y, target_x, target_y);

#if 0
		if(hitcnt > 0 && maneuver_cnt < 2)
		{
			// See what happens if we steer left or right

			int best_hitcnt = 9999;
			int best_drift_idx = 0;
			int best_angle_idx = 0;
			int best_new_x = 0, best_new_y = 0;

			const int side_drifts[12] = {320,-320, 240,-240,200,-200,160,-160,120,-120,80,-80};
			const float drift_angles[4] = {M_PI/6.0, M_PI/8.0, M_PI/12.0, M_PI/16.0};

			int predicted_cur_x = cur_x + cos(ANG32TORAD(cur_ang))*(float)cur_speedlim*2.0;
			int predicted_cur_y = cur_y + sin(ANG32TORAD(cur_ang))*(float)cur_speedlim*2.0;

			for(int angle_idx=0; angle_idx<4; angle_idx++)
			{
				for(int drift_idx=0; drift_idx<12; drift_idx++)
				{
					int new_x, new_y;
					if(side_drifts[drift_idx] > 0)
					{
						new_x = predicted_cur_x + cos(ANG32TORAD(cur_ang)+drift_angles[angle_idx])*side_drifts[drift_idx];
						new_y = predicted_cur_y + sin(ANG32TORAD(cur_ang)+drift_angles[angle_idx])*side_drifts[drift_idx];
					}
					else
					{
						new_x = predicted_cur_x + cos(ANG32TORAD(cur_ang)-drift_angles[angle_idx])*(-1*side_drifts[drift_idx]);
						new_y = predicted_cur_y + sin(ANG32TORAD(cur_ang)-drift_angles[angle_idx])*(-1*side_drifts[drift_idx]);
					}
					int drifted_hitcnt = check_direct_route_hitcnt_mm(cur_ang, new_x, new_y, the_route[route_pos].x, the_route[route_pos].y);
//					printf("a=%.1f deg  drift=%d mm  cur(%d,%d) to(%d,%d)  hitcnt=%d\n",
//						RADTODEG(drift_angles[angle_idx]), side_drifts[drift_idx], predicted_cur_x, predicted_cur_y, new_x, new_y, drifted_hitcnt);
					if(drifted_hitcnt <= best_hitcnt)
					{
						best_hitcnt = drifted_hitcnt;
						best_drift_idx = drift_idx;
						best_angle_idx = angle_idx;
						best_new_x = new_x; best_new_y = new_y;
					}
				}
			}

			if(best_hitcnt < hitcnt && best_hitcnt < 2)
			{

//				do_follow_route = 0;
//				lookaround_creep_reroute = 0;
//				stop_movement();

				if( (abs(side_drifts[best_drift_idx]) < 50) || ( abs(side_drifts[best_drift_idx]) < 100 && drift_angles[best_angle_idx] < M_PI/13.0))
				{
					SPEED(18);
					limit_speed(cur_speedlim);
//					printf("!!!!!!!!!!   Steering is almost needed (not performed) to maintain line-of-sight, hitcnt now = %d, optimum drift = %.1f degs, %d mm (hitcnt=%d), cur(%d,%d) to(%d,%d)\n",
//						hitcnt, RADTODEG(drift_angles[best_angle_idx]), side_drifts[best_drift_idx], best_hitcnt, cur_x, cur_y, best_new_x, best_new_y);
//					if(tcp_client_sock > 0) tcp_send_dbgpoint(cur_x, cur_y, 210, 210,   110, 1);
//					if(tcp_client_sock > 0) tcp_send_dbgpoint(best_new_x, best_new_y,   0, 255,   110, 1);
				}
				else
				{
					printf("Steering is needed, hitcnt now = %d, optimum drift = %.1f degs, %d mm (hitcnt=%d), cur(%d,%d) to(%d,%d)\n", 
						hitcnt, RADTODEG(drift_angles[best_angle_idx]), side_drifts[best_drift_idx], best_hitcnt, cur_x, cur_y, best_new_x, best_new_y);
					if(tcp_client_sock > 0) tcp_send_dbgpoint(cur_x, cur_y, 200, 200,   0, 1);
					if(tcp_client_sock > 0) tcp_send_dbgpoint(best_new_x, best_new_y,   0, 40,   0, 1);
					if(tcp_client_sock > 0) tcp_send_dbgpoint(target_x, target_y, 0, 130, 230, 1);

					// Do the steer
					id_cnt = 0; // id0 is reserved for special maneuvers during route following.
					move_to(best_new_x, best_new_y, 0, (id_cnt<<4) | ((route_pos)&0b1111), 12, 2 /* auto backmode*/);
					send_info((side_drifts[best_drift_idx] > 0)?INFO_STATE_RIGHT:INFO_STATE_LEFT);
					maneuver_cnt++;
				}
			}
			else
			{
//				printf("!!!!!!!!  Steering cannot help in improving line-of-sight.\n");
#endif
				if(hitcnt < 3)
				{
//					printf("!!!!!!!!!!!  Direct line-of-sight to the next point has 1..2 obstacles, slowing down.\n");
					SPEED(18);
					limit_speed(cur_speedlim);
				}
				else
				{
//					printf("Direct line-of-sight to the next point has disappeared! Trying to solve.\n");
					SPEED(18);
					limit_speed(cur_speedlim);
					stop_movement();
					lookaround_creep_reroute = 1;
				}
#if 0
			}
		}
#endif
	}
}

				// End of NAVIGATION
/***********************************************************************************************************************************************************************************************************************************************************************/
/***********************************************************************************************************************************************************************************************************************************************************************/

				// ROUTING THREAD


void routing_thread(thread_struct *p_host_t)
{
//	int ret;
	while(1)
	{
		pthread_mutex_lock(&p_host_t->mutex_token_routing);
		printf("No need for routing now. Routing thread is waiting for a need of routing");
		pthread_cond_wait(&p_host_t->cond_need_routing,&p_host_t->mutex_token_routing); // Gestion des erreurs a faire ? 
		
		p_host_t->no_route_found = run_search(p_host_t->dest_x,p_host_t->dest_y,p_host_t->dont_map_lidars,p_host_t->no_tight);

		printf("Routing done");

		pthread_cond_signal(&p_host_t->cond_routing_done);	// We signal the routing is done. 
		pthread_mutex_unlock(&p_host_t->mutex_token_routing);	

		if(p_host_t->waiting_for_rout_to_end)	// If a command is waiting to be executed after the end of this loop. Wroks with the thread_management.. functions
		{
			p_host_t->rout_thread_on_pause = 1;	// The thread has to be on pause while we run teh command.

			pthread_mutex_lock(&p_host_t->mutex_waiting_rout_t);				

			while(p_host_t->waiting_for_rout_to_end)
			{
				pthread_cond_signal(&p_host_t->cond_routing_done);	// First, we signal the loop has ended
			}
			pthread_mutex_unlock(&p_host_t->mutex_waiting_rout_t);

			sleep(1);
	
			pthread_mutex_lock(&p_host_t->mutex_waiting_rout_t);		
			pthread_cond_wait(&p_host_t->cond_continue_rout, &p_host_t->mutex_waiting_rout_t);	// We wait for the command to be done. 
			pthread_mutex_unlock(&p_host_t->mutex_waiting_rout_t);	
		}

		
	}
	return;
} 

// Search a route to go to a destination. Return 0 = Route found,  Return 1 = No route found
int32_t prev_search_dest_x, prev_search_dest_y;
int run_search(int32_t dest_x, int32_t dest_y, int dont_map_lidars, int no_tight)
{
	send_info(INFO_STATE_THINK);

	prev_search_dest_x = dest_x;
	prev_search_dest_y = dest_y;

	if(!dont_map_lidars)
	{
		int32_t da, dx, dy;
		map_lidars(&world, NUM_LATEST_LIDARS_FOR_ROUTING_START, lidars_to_map_at_routing_start, &da, &dx, &dy);
		INCR_POS_CORR_ID();
		correct_robot_pos(da/2, dx/2, dy/2, pos_corr_id);
	}

	route_unit_t *some_route = NULL;

	int ret = search_route(&world, &some_route, ANG32TORAD(cur_ang), cur_x, cur_y, dest_x, dest_y, no_tight);

	route_unit_t *rt;
	int len = 0;
	DL_FOREACH(some_route, rt)
	{
//		if(rt->backmode)
//			printf(" REVERSE ");
//		else
//			printf("         ");

		int x_mm, y_mm;
		mm_from_unit_coords(rt->loc.x, rt->loc.y, &x_mm, &y_mm);					
//		printf("to %d,%d\n", x_mm, y_mm);

		the_route[len].x = x_mm; the_route[len].y = y_mm; the_route[len].backmode = rt->backmode;
		the_route[len].take_next_early = 100;
		len++;
		if(len >= THE_ROUTE_MAX)
			break;
	}

	for(int i = 0; i < len; i++)
	{
		if(i < len-1)
		{
			float dist = sqrt(sq(the_route[i].x-the_route[i+1].x) + sq(the_route[i].y-the_route[i+1].y));
			int new_early = dist/10;
			if(new_early < 50) new_early = 50;
			else if(new_early > 250) new_early = 250;
			the_route[i].take_next_early = new_early;
		}
	}

	the_route[len-1].take_next_early = 20;

	msg_rc_route_status.num_reroutes++;

	tcp_send_route(cur_x, cur_y, &some_route);

	if(some_route)
	{
		the_route_len = len;
		do_follow_route = 1;
		start_route = 1;
		route_pos = 0;
		route_finished_or_notfound = 0;
		id_cnt++; if(id_cnt > 7) id_cnt = 1;
	}
	else
	{
		do_follow_route = 0;
		route_finished_or_notfound = 1;
		send_info(INFO_STATE_IDLE);
	}

	lookaround_creep_reroute = 0;

	return ret;

}

void send_route_end_status(uint8_t reason)
{
	if(cmd_state == TCP_CR_ROUTE_MID)
	{
		if(tcp_client_sock >= 0)
		{
			msg_rc_route_status.cur_ang = cur_ang>>16;
			msg_rc_route_status.cur_x = cur_x;
			msg_rc_route_status.cur_y = cur_y;
			msg_rc_route_status.status = reason;
			tcp_send_msg(&msgmeta_rc_route_status, &msg_rc_route_status);
		}

		cmd_state = 0;
	}
}



// Not usefull anymore, the routing destination are now stocked in the threading_structure host_t. 
int rerun_search()
{
	return run_search(prev_search_dest_x, prev_search_dest_y, 0, 1);
}



				//end of ROUTING
/***********************************************************************************************************************************************************************************************************************************************************************/
/***********************************************************************************************************************************************************************************************************************************************************************/

				// MAPPING THREAD

// Handles the mapping. Mainly, it calls autofsm(), tof_handling() and lidar_handling().
void mapping_handling(thread_struct *p_host_t)
{
	while(1)
	{
		autofsm();

	#if 0 //def PULUTOF1
		{
			static double prev_incr = 0.0;
			double stamp;
			if( (stamp=subsec_timestamp()) > prev_incr+0.15)
			{
				prev_incr = stamp;

				extern int32_t tof3d_obstacle_levels[3];
				extern pthread_mutex_t cur_pos_mutex;
				int obstacle_levels[3];
				pthread_mutex_lock(&cur_pos_mutex);
				obstacle_levels[0] = tof3d_obstacle_levels[0];
				obstacle_levels[1] = tof3d_obstacle_levels[1];
				obstacle_levels[2] = tof3d_obstacle_levels[2];
				pthread_mutex_unlock(&cur_pos_mutex);

				if(obstacle_levels[2] > 100)
				{
					if(cur_speedlim > 18)
					{
						cur_speedlim = 18;
						limit_speed(cur_speedlim);
					}
				}
				else if(obstacle_levels[2] > 7)
				{
					if(cur_speedlim > 25)
					{
						cur_speedlim = 25;
						limit_speed(cur_speedlim);
					}
				}
				else if(obstacle_levels[1] > 70)
				{
					if(cur_speedlim > 25)
					{
						cur_speedlim = 25;
						limit_speed(cur_speedlim);
					}
				}
				else if(obstacle_levels[1] > 7)
				{
					if(cur_speedlim > 35)
					{
						cur_speedlim = 35;
						limit_speed(cur_speedlim);
					}
				}
				else if(obstacle_levels[0] > 20)
				{
					if(cur_speedlim > 42)
					{
						cur_speedlim = 42;
						limit_speed(cur_speedlim);
					}
				}
				else
				{
					if(cur_speedlim < max_speedlim)
					{
						cur_speedlim++;
						limit_speed(cur_speedlim);
					}
				}
			}
		}

	#else
		{
			static double prev_incr = 0.0;
			double stamp;
			if( (stamp=subsec_timestamp()) > prev_incr+0.15)
			{
				if(cur_speedlim < max_speedlim)
				{
					cur_speedlim++;
					limit_speed(cur_speedlim);
				}
			}
		}



	#endif

		tof_handling();


		{
			static double prev_incr = 0.0;
			double stamp;
			if( (stamp=subsec_timestamp()) > prev_incr+0.15)
			{
				prev_incr = stamp;

				if(cur_speedlim < max_speedlim)
				{
					cur_speedlim++;
					//printf("cur_speedlim++ to %d\n", cur_speedlim);
					limit_speed(cur_speedlim);
				}

				if(cur_speedlim > max_speedlim)
				{
					cur_speedlim--;
					limit_speed(cur_speedlim);
				}
			}
		}


		lidar_handling();

		static uint8_t prev_keep_position;
		if(!state_vect.v.keep_position && prev_keep_position)
			release_motors();
		prev_keep_position = state_vect.v.keep_position;

		static uint8_t prev_autonomous;
		if(state_vect.v.command_source && !prev_autonomous)
		{
			daiju_mode(0);
			routing_set_world(&world);
			start_automapping_skip_compass();
			state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 1;
		}
		if(!state_vect.v.command_source && prev_autonomous)
		{
			stop_automapping();
		}
		prev_autonomous = state_vect.v.command_source;

		static int keepalive_cnt = 0;
		if(++keepalive_cnt > 500)
		{
			keepalive_cnt = 0;
			if(state_vect.v.keep_position)
				send_keepalive();
			else
				release_motors();
		}


		sonar_point_t* p_son;
		if( (p_son = get_sonar()) )
		{
			if(tcp_client_sock >= 0) tcp_send_sonar(p_son);
			if(state_vect.v.mapping_2d)
				map_sonars(&world, 1, p_son);
		}

		static double prev_sync = 0;
		double stamp;

		double write_interval = 30.0;
		if(tcp_client_sock >= 0)
			write_interval = 7.0;

		if( (stamp=subsec_timestamp()) > prev_sync+write_interval)
		{
			prev_sync = stamp;

			int idx_x, idx_y, offs_x, offs_y;
			page_coords(cur_x, cur_y, &idx_x, &idx_y, &offs_x, &offs_y);

			// Do some "garbage collection" by disk-syncing and deallocating far-away map pages.
			unload_map_pages(&world, idx_x, idx_y);

			// Sync all changed map pages to disk
			if(save_map_pages(&world))
			{
				if(tcp_client_sock >= 0) tcp_send_sync_request();
			}
			if(tcp_client_sock >= 0)
			{
				tcp_send_battery();
				tcp_send_statevect();
			}

			fflush(stdout); // syncs log file.

		}



		if(p_host_t->waiting_for_map_to_end)	// If a command is waiting to be executed after the end of this loop. Wroks with the thread_management.. functions
		{
			p_host_t->map_thread_on_pause = 1;	// The thread has to be on pause while we run teh command.
	
			pthread_mutex_lock(&p_host_t->mutex_waiting_map_t);				
	
			while(p_host_t->waiting_for_map_to_end)
			{
				pthread_cond_signal(&p_host_t->cond_mapping_done);	// First, we signal the loop has ended
			}
			pthread_mutex_unlock(&p_host_t->mutex_waiting_map_t);

			sleep(1);
			
			pthread_mutex_lock(&p_host_t->mutex_waiting_map_t);		
			pthread_cond_wait(&p_host_t->cond_continue_map, &p_host_t->mutex_waiting_map_t);	// We wait for the command to be done. 
			pthread_mutex_unlock(&p_host_t->mutex_waiting_map_t);	
		}
	
	} // End of while loop

}



// Handles the tof mapping.
void tof_handling(void)
{
#ifdef PULUTOF1

#ifdef PULUTOF1_GIVE_RAWS

	pulutof_frame_t* p_tof;
	if( (p_tof = get_pulutof_frame()) )
	{
		if(tcp_client_sock >= 0)
		{
#ifdef PULUTOF_EXTRA
			tcp_send_picture(p_tof->dbg_id, 2, 160, 60, p_tof->dbg);
#endif
			tcp_send_picture(100,           2, 160, 60, (uint8_t*)p_tof->depth);
#ifdef PULUTOF_EXTRA
			tcp_send_picture(110,           2, 160, 60, (uint8_t*)p_tof->uncorrected_depth);
#endif
		}

	}

#else
	tof3d_scan_t *p_tof;

	if( (p_tof = get_tof3d()) )
	{

		if(tcp_client_sock >= 0)
		{
			static int hmap_cnt = 0;
			hmap_cnt++;

			if(hmap_cnt >= 4)
			{
				tcp_send_hmap(TOF3D_HMAP_XSPOTS, TOF3D_HMAP_YSPOTS, p_tof->robot_pos.ang, p_tof->robot_pos.x, p_tof->robot_pos.y, TOF3D_HMAP_SPOT_SIZE, p_tof->objmap);

				if(send_raw_tof >= 0 && send_raw_tof < 4)
				{
					tcp_send_picture(100, 2, 160, 60, (uint8_t*)p_tof->raw_depth);
					tcp_send_picture(101, 2, 160, 60, (uint8_t*)p_tof->ampl_images[send_raw_tof]);
				}

				hmap_cnt = 0;

				if(send_pointcloud)
				{
					save_pointcloud(p_tof->n_points, p_tof->cloud);
				}
			}
		}

		static int32_t prev_x, prev_y, prev_ang;

		if(!flush_3dtof && state_vect.v.mapping_3d && !pwr_status.charging && !pwr_status.charged)
		{
			if(p_tof->robot_pos.x != 0 || p_tof->robot_pos.y != 0 || p_tof->robot_pos.ang != 0)
			{
				int robot_moving = 0;
				if((prev_x != p_tof->robot_pos.x || prev_y != p_tof->robot_pos.y || prev_ang != p_tof->robot_pos.ang))
				{
					prev_x = p_tof->robot_pos.x; prev_y = p_tof->robot_pos.y; prev_ang = p_tof->robot_pos.ang;
					robot_moving = 1;
				}

				static int n_tofs_to_map = 0;
				static tof3d_scan_t* tofs_to_map[25];

				tofs_to_map[n_tofs_to_map] = p_tof;
				n_tofs_to_map++;

				if(n_tofs_to_map >= (robot_moving?3:20))
				{
					int32_t mid_x, mid_y;
					map_3dtof(&world, n_tofs_to_map, tofs_to_map, &mid_x, &mid_y);

					if(do_follow_route)
					{
						int px, py, ox, oy;
						page_coords(mid_x, mid_y, &px, &py, &ox, &oy);

						for(int ix=-1; ix<=1; ix++)
						{
							for(int iy=-1; iy<=1; iy++)
							{
								gen_routing_page(&world, px+ix, py+iy, 0);
							}
						}
					}

					n_tofs_to_map = 0;
				}
			}
		}

		if(flush_3dtof) flush_3dtof--;

	}
#endif
#endif

}



// Handles lidar mapping
void lidar_handling(void)
{
	lidar_scan_t* p_lid;

	if( (p_lid = get_significant_lidar()) || (p_lid = get_basic_lidar()) )
	{
		static int hwdbg_cnt = 0;
		hwdbg_cnt++;
		if(hwdbg_cnt > 0)
		{
			if(tcp_client_sock >= 0) tcp_send_hwdbg(hwdbg);
			hwdbg_cnt = 0;
		}

		static int lidar_send_cnt = 0;
		lidar_send_cnt++;
		if(lidar_send_cnt > 3)
		{
			if(tcp_client_sock >= 0) tcp_send_lidar_lowres(p_lid);
			lidar_send_cnt = 0;
		}

		static int lidar_ignore_cnt = 0;

		if(p_lid->id != pos_corr_id)
		{

			lidar_ignore_cnt++;

	//				if(p_lid->significant_for_mapping) 
	//				printf("Ignoring lidar scan with id=%d (significance=%d).\n", p_lid->id, p_lid->significant_for_mapping);

			if(lidar_ignore_cnt > 20)
			{
				lidar_ignore_cnt = 0;
				printf("WARN: lidar id was stuck, fixing...\n");
				INCR_POS_CORR_ID();
				correct_robot_pos(0, 0, 0, pos_corr_id);

			}
		}
		else
		{
			lidar_ignore_cnt = 0;
			lidar_ignore_over = 1;

			int idx_x, idx_y, offs_x, offs_y;
			//printf("Got lidar scan. (%d)\n", p_lid->significant_for_mapping);

			static int curpos_send_cnt = 0;
			curpos_send_cnt++;
			if(curpos_send_cnt > 2)
			{
				if(tcp_client_sock >= 0)
				{
					msg_rc_pos.ang = cur_ang>>16;
					msg_rc_pos.x = cur_x;
					msg_rc_pos.y = cur_y;
					msg_rc_pos.cmd_state = cmd_state;
					tcp_send_msg(&msgmeta_rc_pos, &msg_rc_pos);
				}
				curpos_send_cnt = 0;
			}

			page_coords(p_lid->robot_pos.x, p_lid->robot_pos.y, &idx_x, &idx_y, &offs_x, &offs_y);
			load_25pages(&world, idx_x, idx_y);

			if(state_vect.v.mapping_collisions)
			{
				// Clear any walls and items within the robot:
				clear_within_robot(&world, p_lid->robot_pos);
			}


			// Keep a pointer list of a few latest lidars; significant or insignificant will do.
			// This list is used to do last-second mapping before routing, to get good starting position.
			for(int i = NUM_LATEST_LIDARS_FOR_ROUTING_START-1; i >= 1; i--)
			{
				lidars_to_map_at_routing_start[i] = lidars_to_map_at_routing_start[i-1];
			}
			lidars_to_map_at_routing_start[0] = p_lid;

			if(p_lid->significant_for_mapping & map_significance_mode)
			{
	//					lidar_send_cnt = 0;
	//					if(tcp_client_sock >= 0) tcp_send_lidar(p_lid);

				static int n_lidars_to_map = 0;
				static lidar_scan_t* lidars_to_map[20];

				if(p_lid->is_invalid)
				{
					if(n_lidars_to_map < 3)
					{
						printf("Got DISTORTED significant lidar scan, have too few lidars -> mapping queue reset\n");
						n_lidars_to_map = 0;
					}
					else
					{
						printf("Got DISTORTED significant lidar scan, running mapping early on previous images\n");
						int32_t da, dx, dy;

						map_lidars(&world, n_lidars_to_map, lidars_to_map, &da, &dx, &dy);
						INCR_POS_CORR_ID();
						correct_robot_pos(da/3, dx/3, dy/3, pos_corr_id);

						n_lidars_to_map = 0;
					}
				}
				else
				{
					//printf("Got significant(%d) lidar scan, adding to the mapping queue(%d).\n", p_lid->significant_for_mapping, n_lidars_to_map);
					lidars_to_map[n_lidars_to_map] = p_lid;

					n_lidars_to_map++;


					if((state_vect.v.localize_with_big_search_area && n_lidars_to_map > 11) ||
					   (!state_vect.v.localize_with_big_search_area &&
						((good_time_for_lidar_mapping && n_lidars_to_map > 3) || n_lidars_to_map > 4)))
					{
						if(good_time_for_lidar_mapping) good_time_for_lidar_mapping = 0;
						int32_t da, dx, dy;

						map_lidars(&world, n_lidars_to_map, lidars_to_map, &da, &dx, &dy);
						INCR_POS_CORR_ID();

						if(state_vect.v.localize_with_big_search_area)
							correct_robot_pos(da, dx, dy, pos_corr_id);
						else
							correct_robot_pos(da/2, dx/2, dy/2, pos_corr_id);
						n_lidars_to_map = 0;
					}
				}

			}

		}

	}
}



/***********************************************************************************************************************************************************************************************************************************************************************/
/***********************************************************************************************************************************************************************************************************************************************************************/
				// COMUNICATION THREAD


/* This function handles both communications : 

1) The devs commands wich can be used directly from the raspberry console (host standard input). The point is to be able to act on the host with these simple commands. After receiving the command from the dev, it will be treated by cmd_from_developer_to_host(cmd).

2) The TCP/IP communication between the Host and the client. After handling what comes from the client through the TCP/IP socket (handling_tcp_client()), we will treat the command from the user thanks to 


*/
void communication_handling(thread_struct *p_host_t)
{

	unsigned char priority_bits = 0x00; // The last 3 bits of this will define if we have to delete or ait for the end of the running threads
/*
	0x00 = The new comand is not critical for any thread, not thread will be canceled, we'll just wait for them to end 
	0x01 = The first bit concerns the mapping thread. If it is on 1, then it is critical for the mapping to run the comand asap. We'll delete the mapping thread, run the comand and finaly recreate a mapping threa.
	0x02 = The second bit concerns the routing thread. If it is on 1, same behaviour than mapping
	0x04 = The third bit concerns the navigation. Same.
There are still 5 bits available on that Bytes that could be used for future Threads. 
*/

	while(1)
	{

	// Calculate fd_set size (biggest fd+1)
		int fds_size = 	
#ifdef SIMULATE_SERIAL
			0;
#else		
			uart;
#endif
		if(tcp_listener_sock > fds_size) fds_size = tcp_listener_sock;
		if(tcp_client_sock > fds_size) fds_size = tcp_client_sock;
		if(STDIN_FILENO > fds_size) fds_size = STDIN_FILENO;
		fds_size+=1;


		fd_set fds;
		FD_ZERO(&fds);   // Put the file descriptors of uart, tcp/ip socket file descriptor in fds
#ifndef SIMULATE_SERIAL
		FD_SET(uart, &fds);
#endif
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(tcp_listener_sock, &fds);
		if(tcp_client_sock >= 0)
			FD_SET(tcp_client_sock, &fds);

		struct timeval select_time = {0, 200};

		if(select(fds_size, &fds, NULL, NULL, &select_time) < 0)
		{
			fprintf(stderr, "select() error %d", errno);
			return NULL;
		}


// *********************** Wait for a command***************************************



		if(FD_ISSET(STDIN_FILENO, &fds))	// 1) Console commands
		{
			int cmd = fgetc(stdin);
	//		cmd_from_developer_to_host(cmd, &p_host_t);  // For some reason I cannot compile the code with this line...
			

#ifndef SIMULATE_SERIAL	
		if(FD_ISSET(uart, &fds))	// UART Handling
		{
			handle_uart();
		}
#endif

		if(tcp_client_sock >= 0 && FD_ISSET(tcp_client_sock, &fds))	// 2) TCP/IP commands from client
		{
			int ret = handle_tcp_client();
			cmd_state = ret;
	
			thread_management_before_running_cmd(priority_bits, &p_host_t);

			cmd_from_client_to_host(ret, &p_host_t);	//Run the comand

			thread_management_after_running_cmd(&p_host_t);
		}

		if(FD_ISSET(tcp_listener_sock, &fds))	// Handling the communication from the host to the client/server, go to tcp_parser.c for more info.
		{
			handle_tcp_listener();
		}

//		static int prev_compass_ang = 0;

//		if(cur_compass_ang != prev_compass_ang)
//		{
//			prev_compass_ang = cur_compass_ang;
//			printf("Compass ang=%.1f deg\n", ANG32TOFDEG(cur_compass_ang));
//		}

		static int micronavi_stop_flags_printed = 0;

		if(cmd_state == TCP_CR_DEST_MID)
		{
			if(cur_xymove.remaining < 5)
			{
				if(tcp_client_sock >= 0)
				{
					msg_rc_movement_status.cur_ang = cur_ang>>16;
					msg_rc_movement_status.cur_x = cur_x;
					msg_rc_movement_status.cur_y = cur_y;
					msg_rc_movement_status.status = TCP_RC_MOVEMENT_STATUS_SUCCESS;
					msg_rc_movement_status.obstacle_flags = 0;
					tcp_send_msg(&msgmeta_rc_movement_status, &msg_rc_movement_status);
				}

				cmd_state = 0;

			}
		}
	

		if(cur_xymove.micronavi_stop_flags)		
		{
			if(!micronavi_stop_flags_printed)
			{
				micronavi_stop_flags_printed = 1;
				printf("MCU-level micronavigation: STOP. Reason flags:\n");
				for(int i=0; i<32; i++)
				{
					if(cur_xymove.micronavi_stop_flags&(1UL<<i))
					{
						printf("bit %2d: %s\n", i, MCU_NAVI_STOP_NAMES[i]);
					}
				}

				printf("Actions being taken:\n");
				for(int i=0; i<32; i++)
				{
					if(cur_xymove.micronavi_action_flags&(1UL<<i))
					{
						printf("bit %2d: %s\n", i, MCU_NAVI_ACTION_NAMES[i]);
					}
				}

				printf("\n");

				if(cmd_state == TCP_CR_DEST_MID)
				{
					if(tcp_client_sock >= 0)
					{
						msg_rc_movement_status.cur_ang = cur_ang>>16;
						msg_rc_movement_status.cur_x = cur_x;
						msg_rc_movement_status.cur_y = cur_y;
						msg_rc_movement_status.status = TCP_RC_MOVEMENT_STATUS_STOPPED;
						msg_rc_movement_status.obstacle_flags = cur_xymove.micronavi_stop_flags;
						tcp_send_msg(&msgmeta_rc_movement_status, &msg_rc_movement_status);
					}

					cmd_state = 0;
				}
			}
		}
		else
			micronavi_stop_flags_printed = 0;

		static int feedback_stop_flags_processed = 0;

		if(cur_xymove.feedback_stop_flags)
		{
			if(!feedback_stop_flags_processed)
			{
				feedback_stop_flags_processed = 1;
				int stop_reason = cur_xymove.feedback_stop_flags;
				printf("Feedback module reported: %s\n", MCU_FEEDBACK_COLLISION_NAMES[stop_reason]);
				if(state_vect.v.mapping_collisions)
				{
					map_collision_obstacle(&world, cur_ang, cur_x, cur_y, stop_reason, cur_xymove.stop_xcel_vector_valid,
						cur_xymove.stop_xcel_vector_ang_rad);
					if(do_follow_route) // regenerate routing pages because the map is changed now.
					{
						int px, py, ox, oy;
						page_coords(cur_x, cur_y, &px, &py, &ox, &oy);

						for(int ix=-1; ix<=1; ix++)
						{
							for(int iy=-1; iy<=1; iy++)
							{
								gen_routing_page(&world, px+ix, py+iy, 0);
							}
						}
					}
				}
				if(cmd_state == TCP_CR_DEST_MID)
				{
					if(tcp_client_sock >= 0)
					{
						msg_rc_movement_status.cur_ang = cur_ang>>16;
						msg_rc_movement_status.cur_x = cur_x;
						msg_rc_movement_status.cur_y = cur_y;
						msg_rc_movement_status.status = TCP_RC_MOVEMENT_STATUS_STOPPED_BY_FEEDBACK_MODULE;
						msg_rc_movement_status.obstacle_flags = cur_xymove.feedback_stop_flags;
						tcp_send_msg(&msgmeta_rc_movement_status, &msg_rc_movement_status);
					}

					cmd_state = 0;
				}

			}
		}
		else
			feedback_stop_flags_processed = 0;
	} // end of while(1) loop

}


// Treat the commands from the standard input.
void cmd_from_developer_to_host(int cmd, thread_struct **pp_host_t)
{

#ifdef MOTCON_PID_EXPERIMENT
		static uint8_t pid_i_max = 30;
		static uint8_t pid_feedfwd = 30;
		static uint8_t pid_p = 80;
		static uint8_t pid_i = 80;
		static uint8_t pid_d = 50;
#endif

	if(cmd == 'q')
	{
		retval = 0;
	}
	if(cmd == 'Q')
	{
		retval = 5;
	}

	if(cmd == 'S')
	{
		save_robot_pos();
	}
	if(cmd == 's')
	{
		retrieve_robot_pos();
	}

/*			if(cmd == 'c')
	{
		printf("Starting automapping from compass round.\n");
		routing_set_world(&world);
		start_automapping_from_compass();
	}
	if(cmd == 'a')
	{
		printf("Starting automapping, skipping compass round.\n");
		routing_set_world(&world);
		start_automapping_skip_compass();
	}
	if(cmd == 'w')
	{
		printf("Stopping automapping.\n");
		stop_automapping();
	}
*/			if(cmd == '0')
	{
		set_robot_pos(0,0,0);
	}
	if(cmd == 'M')
	{
		printf("Requesting massive search.\n");
		state_vect.v.localize_with_big_search_area = 2;
	}
	if(cmd == 'L')
	{
		conf_charger_pos();
	}
	if(cmd == 'l')
	{
		hw_find_charger();

//				read_charger_pos();
//				find_charger_state = 1;
	}
	if(cmd == 'v')
	{
		if(state_vect.v.keep_position)
		{
			state_vect.v.keep_position = 0;
			printf("Robot is free to move manually.\n");
		}
		else
		{
			state_vect.v.keep_position = 1;
			printf("Robot motors enabled again.\n");
		}
	}

	if(cmd == 'V')
	{
		verbose_mode = verbose_mode?0:1;
	}

#ifdef PULUTOF1
	if(cmd == 'z')
	{
		pulutof_decr_dbg();
	}
	if(cmd == 'x')
	{
		pulutof_incr_dbg();
	}
	if(cmd == 'Z')
	{
		if(send_raw_tof >= 0) send_raw_tof--;
		printf("Sending raw tof from sensor %d\n", send_raw_tof);
	}
	if(cmd == 'X')
	{
		if(send_raw_tof < 3) send_raw_tof++;
		printf("Sending raw tof from sensor %d\n", send_raw_tof);
	}
	if(cmd >= '1' && cmd <= '4')
	{
		pulutof_cal_offset(cmd-'1');
	}
	if(cmd == 'p')
	{
		if(send_pointcloud == 0)
		{
			printf("INFO: Will send pointclouds relative to robot origin\n");
			send_pointcloud = 1;
		}
		else if(send_pointcloud == 1)
		{
			printf("INFO: Will send pointclouds relative to world origin\n");
			send_pointcloud = 2;
		}
		else
		{
			printf("INFO: Will stop sending pointclouds\n");
			send_pointcloud = 0;
		}
	}
#endif

#if 0
	if(cmd >= '1' && cmd <= '9')
	{
		uint8_t bufings[3];
		bufings[0] = 0xd0 + cmd-'0';
		bufings[1] = 0;
		bufings[2] = 0xff;
		printf("Sending dev msg: %x\n", bufings[0]);
		send_uart(bufings, 3);				
	}
#endif

#ifdef MOTCON_PID_EXPERIMENT
	if(cmd == 'A') {int tmp = (int)pid_i_max*5/4; if(tmp>255) tmp=255; pid_i_max=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'a') {int tmp = (int)pid_i_max*3/4; if(tmp<4) tmp=4;     pid_i_max=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'S') {int tmp = (int)pid_feedfwd*5/4; if(tmp>255) tmp=255; pid_feedfwd=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 's') {int tmp = (int)pid_feedfwd*3/4; if(tmp<3) tmp=4;     pid_feedfwd=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'D') {int tmp = (int)pid_p*5/4; if(tmp>255) tmp=255; pid_p=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'd') {int tmp = (int)pid_p*3/4; if(tmp<4) tmp=4;     pid_p=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'F') {int tmp = (int)pid_i*5/4; if(tmp>255) tmp=255; pid_i=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'f') {int tmp = (int)pid_i*3/4; if(tmp<4) tmp=4;     pid_i=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'G') {int tmp = (int)pid_d*5/4; if(tmp>255) tmp=255; pid_d=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'g') {int tmp = (int)pid_d*3/4; if(tmp<4) tmp=4;     pid_d=tmp; send_motcon_pid(pid_i_max, pid_feedfwd, pid_p, pid_i, pid_d);}
	if(cmd == 'z') {turn_and_go_rel_rel(0, 2000, 25, 1);}
	if(cmd == 'Z') {turn_and_go_rel_rel(0, -2000, 25, 1);}
#endif
	}
}


// Treat the commands from the client
void cmd_from_client_to_host(int cmd, thread_struct **pp_host_t)
{
	if(cmd == TCP_CR_DEST_MID)
	{
		state_vect.v.keep_position = 1;
		daiju_mode(0);

		msg_rc_movement_status.start_ang = cur_ang>>16;
		msg_rc_movement_status.start_x = cur_x;
		msg_rc_movement_status.start_y = cur_y;

		msg_rc_movement_status.requested_x = msg_cr_dest.x;
		msg_rc_movement_status.requested_y = msg_cr_dest.y;
		msg_rc_movement_status.requested_backmode = msg_cr_dest.backmode;

		cur_xymove.remaining = 999999; // invalidate

		printf("  ---> DEST params: X=%d Y=%d backmode=0x%02x\n", msg_cr_dest.x, msg_cr_dest.y, msg_cr_dest.backmode);
		if(msg_cr_dest.backmode & 0b1000) // Rotate pose
		{
			float ang = atan2(msg_cr_dest.y-cur_y, msg_cr_dest.x-cur_x);
			turn_and_go_abs_rel(RADTOANG32(ang), 0, cur_speedlim, 1);
		}
		else
			move_to(msg_cr_dest.x, msg_cr_dest.y, msg_cr_dest.backmode, 0, cur_speedlim, 1);

		find_charger_state = 0;
		lookaround_creep_reroute = 0;
		do_follow_route = 0;
		send_info(INFO_STATE_IDLE);

	}
	else if(cmd == TCP_CR_ROUTE_MID)
	{			

		printf("  ---> ROUTE params: X=%d Y=%d dummy=%d\n", msg_cr_route.x, msg_cr_route.y, msg_cr_route.dummy);

		msg_rc_route_status.start_ang = cur_ang>>16;
		msg_rc_route_status.start_x = cur_x;
		msg_rc_route_status.start_y = cur_y;

		msg_rc_route_status.requested_x = msg_cr_route.x;
		(**pp_host_t).dest_x = msg_cr_route.x;
		msg_rc_route_status.requested_y = msg_cr_route.y;
		(**pp_host_t).dest_y = msg_cr_route.y;
		msg_rc_route_status.status = TCP_RC_ROUTE_STATUS_UNDEFINED;
		msg_rc_route_status.num_reroutes = -1;

		state_vect.v.keep_position = 1;
		daiju_mode(0);
		find_charger_state = 0;
		
		pthread_cond_signal(&(**pp_host_t).cond_need_routing);	// Asking for a routing to the host_t.dest_x/y
		pthread_cond_wait(&(**pp_host_t).cond_routing_done, &(**pp_host_t).mutex_token_routing);

		if((**pp_host_t).no_route_found != 0)
		{
			send_route_end_status((**pp_host_t).no_route_found);
		}
	}
	else if(cmd == TCP_CR_CHARGE_MID)
	{
		read_charger_pos();
		find_charger_state = 1;
	}
	else if(cmd == TCP_CR_ADDCONSTRAINT_MID)
	{
		printf("  ---> ADD CONSTRAINT params: X=%d Y=%d\n", msg_cr_addconstraint.x, msg_cr_addconstraint.y);
		add_map_constraint(&world, msg_cr_addconstraint.x, msg_cr_addconstraint.y);
	}
	else if(cmd == TCP_CR_REMCONSTRAINT_MID)
	{
		printf("  ---> REMOVE CONSTRAINT params: X=%d Y=%d\n", msg_cr_remconstraint.x, msg_cr_remconstraint.y);
		for(int xx=-2; xx<=2; xx++)
		{
			for(int yy = -2; yy<=2; yy++)
			{
				remove_map_constraint(&world, msg_cr_remconstraint.x + xx*40, msg_cr_remconstraint.y + yy*40);
			}
		}
	}
	else if(cmd == TCP_CR_MODE_MID)	// Most mode messages deprecated, here for backward-compatibility, will be removed soon.
	{
		printf("Request for MODE %d\n", msg_cr_mode.mode);
		switch(msg_cr_mode.mode)
		{
			case 0:
			{
				state_vect.v.keep_position = 1;
				daiju_mode(0);
				stop_automapping();
				state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 0;
			} break;

			case 1:
			{
				state_vect.v.keep_position = 1;
				daiju_mode(0);
				stop_automapping();
				find_charger_state = 0;
				lookaround_creep_reroute = 0;
				do_follow_route = 0;
				send_info(INFO_STATE_IDLE);
				state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 1;

			} break;

			case 2:
			{
				state_vect.v.keep_position = 1;
				daiju_mode(0);
				routing_set_world(&world);
				start_automapping_skip_compass();
				state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 1;
			} break;

			case 3:
			{
				state_vect.v.keep_position = 1;
				daiju_mode(0);
				routing_set_world(&world);
				start_automapping_from_compass();
				state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 1;
			} break;

			case 4:
			{
				stop_automapping();
				find_charger_state = 0;
				lookaround_creep_reroute = 0;
				do_follow_route = 0;
				state_vect.v.keep_position = 1;
				send_info(INFO_STATE_DAIJUING);
				daiju_mode(1);
				state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 0;
			} break;

			case 5:
			{
				stop_automapping();
				find_charger_state = 0;
				lookaround_creep_reroute = 0;
				do_follow_route = 0;
				send_info(INFO_STATE_IDLE);
				state_vect.v.keep_position = 0;
				release_motors();
				state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 1;
			} break;

			case 6:
			{
				stop_automapping();
				find_charger_state = 0;
				lookaround_creep_reroute = 0;
				send_info(INFO_STATE_IDLE);
				do_follow_route = 0;
				state_vect.v.keep_position = 0;
				release_motors();
				state_vect.v.mapping_collisions = state_vect.v.mapping_3d = state_vect.v.mapping_2d = state_vect.v.loca_3d = state_vect.v.loca_2d = 0;
			} break;

			case 7:
			{
				conf_charger_pos();
			} break;

			case 8:
			{
				stop_automapping();
				find_charger_state = 0;
				lookaround_creep_reroute = 0;
				do_follow_route = 0;
				stop_movement();
				send_info(INFO_STATE_IDLE);
			} break;

			case 9:
			{
				
			} break;


			default: break;
		}
	}
	else if(cmd == TCP_CR_MANU_MID)
	{
		#define MANU_FWD   10
		#define MANU_BACK  11
		#define MANU_LEFT  12
		#define MANU_RIGHT 13
		stop_automapping();
		daiju_mode(0);
		state_vect.v.keep_position = 1;
		printf("Manual OP %d\n", msg_cr_manu.op);
		switch(msg_cr_manu.op)
		{
			case MANU_FWD:
				turn_and_go_abs_rel(cur_ang, 100, 10, 1);
			break;
			case MANU_BACK:
				turn_and_go_abs_rel(cur_ang, -100, 10, 1);
			break;
			case MANU_LEFT:
				turn_and_go_abs_rel(cur_ang-10*ANG_1_DEG, 0, 10, 1);
			break;
			case MANU_RIGHT:
				turn_and_go_abs_rel(cur_ang+10*ANG_1_DEG, 0, 10, 1);
			break;
			default:
			break;
		}
	}		
	else if(cmd == TCP_CR_MAINTENANCE_MID)
	{
		if(msg_cr_maintenance.magic == 0x12345678)
		{
			retval = msg_cr_maintenance.retval;
		}
		else
		{
			printf("WARN: Illegal maintenance message magic number 0x%08x.\n", msg_cr_maintenance.magic);
		}
	}		
	else if(cmd == TCP_CR_SPEEDLIM_MID)
	{
		int new_lim = msg_cr_speedlim.speedlim_linear_fwd;
		printf("INFO: Speedlim msg %d\n", new_lim);
		if(new_lim < 1 || new_lim > MAX_CONFIGURABLE_SPEEDLIM)
			max_speedlim = DEFAULT_SPEEDLIM;
		else
			max_speedlim = new_lim;

		if(cur_speedlim > max_speedlim)
		{
			cur_speedlim = max_speedlim;
			limit_speed(cur_speedlim);
		}
	}
	else if(cmd == TCP_CR_STATEVECT_MID)
	{
		tcp_send_statevect();
	}
	else if(cmd == TCP_CR_SETPOS_MID)
	{
		set_robot_pos(msg_cr_setpos.ang<<16, msg_cr_setpos.x, msg_cr_setpos.y);

		INCR_POS_CORR_ID();
		correct_robot_pos(0, 0, 0, pos_corr_id); // forces new LIDAR ID, so that correct amount of images (on old coords) are ignored

#ifdef PULUTOF1
		while(get_tof3d()); // flush 3DTOF queue
#endif
		flush_3dtof = 2; // Flush two extra scans
	}
}


// When a command comes in from the client, it is most of the time to affect the behaviour of the robot. We have to manage the thread(s) concerned by the command
// to accomplish the task the user asked. 
// Some commands are more important than others and should be run asap (They have a priority), that leads to cancel the running threads concerned by this comand.
// If it is not critical to run the command immediatly, then we ll just wait until the concerned thread(s) end their main loop, stop it/them, 
// run the command and then resume it/them.
void thread_management_before_running_cmd(unsigned char priority_bits,thread_struct** pp_host_t)
{
	// int ret; 
	
	// Mapping thread managing
	if((priority_bits & 0x01) == 1)  // If the new comand concerns the Mapping thread and has priority over what the robot is already doing.
	{
		if((**pp_host_t).map_thread_cancel_state == 1)	// The map_thread_cancel_state defines if the mapping thread can be canceled. If not, we ll wait until the end of the thread loop.
		{
			if(pthread_cancel((**pp_host_t).thread_mapping) != 0)	// When the cancel is successfull, it returns 0.
			{
				printf("Error canceling mappping Thread.");
			}
			else
			{
				(**pp_host_t).map_thread_were_canceled = 1;
				printf("Mapping thread canceled successfully.");
			}
		}
		else 
		{
			printf("The mapping Thread cannot be canceled now, we'll wait until it ends."); // If it cannot ne canceled
			(**pp_host_t).waiting_for_map_to_end = 1;	// This flag indicates we are waiting the end of the mapping thread loop.	
									// The flag is used at the end of the thread so it stops and send the "work done" signal
			pthread_mutex_lock(&(**pp_host_t).mutex_waiting_map_t); // Waiting the signal that it has ended
			pthread_cond_wait(&(**pp_host_t).cond_mapping_done, &(**pp_host_t).mutex_waiting_map_t);
			
			(**pp_host_t).waiting_for_map_to_end = 0;

			pthread_mutex_unlock(&(**pp_host_t).mutex_waiting_map_t);
			
			printf("Mapping thread loop has ended, now on pause, run the command and resume it.");
		}

	}
	else	// If it has no priority, we'll wait until the end of the mapping thread
	{
		(**pp_host_t).waiting_for_map_to_end = 1; // Flag up
		
		pthread_mutex_lock(&(**pp_host_t).mutex_waiting_map_t);	// Waiting the signal that it has ended
		pthread_cond_wait(&(**pp_host_t).cond_mapping_done, &(**pp_host_t).mutex_waiting_map_t);
		
		(**pp_host_t).waiting_for_map_to_end = 0;
		

		pthread_mutex_unlock(&(**pp_host_t).mutex_waiting_map_t);
		
		printf("Mapping thread loop has ended, now on pause, run the command and resume it..");
	}

	// Navigation thread managing, it works the same than the mapping one.
	if((priority_bits & 0x04) == 1)  // If the new comand concerns the Navigation thread and has priority over what the robot is already doing.
	{
		if((**pp_host_t).nav_thread_cancel_state == 1)	
		{
			if(pthread_cancel((**pp_host_t).thread_navigation) != 0)
			{
				printf("Error canceling navigation Thread.");
			}
			else
			{
				(**pp_host_t).nav_thread_were_canceled = 1;
				printf("Navigation thread canceled successfully.");
			}
		}
		else 
		{
			printf("The navigation Thread cannot be canceled now, we'll wait until it ends.");
			(**pp_host_t).waiting_for_nav_to_end = 1;	// This flag indicates we are waiting the end of the navigation thread loop.
			
			pthread_mutex_lock(&(**pp_host_t).mutex_waiting_nav_t);
			pthread_cond_wait(&(**pp_host_t).cond_navigation_done, &(**pp_host_t).mutex_waiting_nav_t);
			
			(**pp_host_t).waiting_for_nav_to_end = 0;
			
			pthread_mutex_unlock(&(**pp_host_t).mutex_waiting_nav_t);
			
			printf("Navigation thread loop has ended, now on pause, run the command and resume it.");
		}

	}
	else	// If it has no priority, we'll wait until the end of the mapping thread
	{
		(**pp_host_t).waiting_for_nav_to_end = 1;
		
		pthread_mutex_lock(&(**pp_host_t).mutex_waiting_nav_t);
		pthread_cond_wait(&(**pp_host_t).cond_navigation_done, &(**pp_host_t).mutex_waiting_nav_t);
		
		(**pp_host_t).waiting_for_nav_to_end = 0;
		
		(**pp_host_t).map_thread_on_pause = 1;

		pthread_mutex_unlock(&(**pp_host_t).mutex_waiting_nav_t);
		
		printf("Navigation thread loop has ended, now on pause, run the command and resume it..");
	}


	// Routing thread managing
	if((priority_bits & 0x01) == 1)  // Same as before but with the routing thread
	{
		if((**pp_host_t).rout_thread_cancel_state == 1)	// The routing_thread_cancel_state defines if the routing thread can be canceled. 
		{						// If not, we ll wait until the end of the thread loop.
			if(pthread_cancel((**pp_host_t).thread_routing) != 0)	// When the cancel is successfull, it returns 0.
			{
				printf("Error canceling routing Thread.");
			}
			else
			{
				(**pp_host_t).rout_thread_were_canceled = 1;
				printf("Routing thread canceled successfully.");
			}
		}
		else 
		{
			printf("The routing Thread cannot be canceled now, we'll wait until it ends."); // If it cannot ne canceled
			(**pp_host_t).waiting_for_rout_to_end = 1;	// This flag indicates we are waiting the end of the routing thread loop.	
									// The flag is used at the end of the thread so it stops and send the "work done" signal
			pthread_mutex_lock(&(**pp_host_t).mutex_waiting_rout_t); // Waiting the signal that it has ended
			pthread_cond_wait(&(**pp_host_t).cond_routing_done, &(**pp_host_t).mutex_waiting_rout_t);
			
			(**pp_host_t).waiting_for_rout_to_end = 0;

			pthread_mutex_unlock(&(**pp_host_t).mutex_waiting_rout_t);
			
			printf("Routing thread loop has ended, now on pause, run the command and resume it.");
		}

	}
	else	// If it has no priority, we'll wait until the end of the routing thread
	{
		(**pp_host_t).waiting_for_rout_to_end = 1; // Flag up
		
		pthread_mutex_lock(&(**pp_host_t).mutex_waiting_rout_t);	// Waiting the signal that it has ended
		pthread_cond_wait(&(**pp_host_t).cond_routing_done, &(**pp_host_t).mutex_waiting_rout_t);
		
		(**pp_host_t).waiting_for_rout_to_end = 0;
		

		pthread_mutex_unlock(&(**pp_host_t).mutex_waiting_rout_t);
		
		printf("Routing thread loop has ended, now on pause, run the command and resume it..");
	}
	return;

}


// After running the command we have to remanage the threads according to what have been done before the command (thread_management_before_running_cmd())
// If threads were canceled, then we recreate them, if some are on pause, we resume them.  
int thread_management_after_running_cmd(thread_struct** pp_host_t)
{
	int ret;	
	
	if((**pp_host_t).map_thread_on_pause == 1)	// If one of the threads are on pause, we resume them.
	{
		(**pp_host_t).map_thread_on_pause = 0;
		pthread_cond_signal(&(**pp_host_t).cond_continue_map);
	}

	if((**pp_host_t).nav_thread_on_pause == 1)
	{
		(**pp_host_t).nav_thread_on_pause = 0;
		pthread_cond_signal(&(**pp_host_t).cond_continue_nav);
	}

	if((**pp_host_t).rout_thread_on_pause == 1)
	{
		(**pp_host_t).rout_thread_on_pause = 0;
		pthread_cond_signal(&(**pp_host_t).cond_continue_rout);
	}

	if((**pp_host_t).map_thread_were_canceled == 1)
	{
		(**pp_host_t).map_thread_on_pause = 0;
		// Mapping Thread creation
		if((ret = pthread_create(&(**pp_host_t).thread_mapping, NULL, mapping_handling, &(**pp_host_t))) != 0)  // I have doubt if the parameter is right
		{
			printf("ERROR: maping thread creation failed, ret = %d\n", ret);
			return -1;
		}	
	}

	if((**pp_host_t).nav_thread_were_canceled == 1)		// If one of the threads were canceled, we recreate it.
	{
		(**pp_host_t).nav_thread_on_pause = 0;
		// Navigation Thread creation
		if((ret = pthread_create(&(**pp_host_t).thread_navigation, NULL, route_fsm, &(**pp_host_t))) != 0) 
		{
			printf("ERROR: navigation thread creation failed, ret = %d\n", ret);
			return -1;
		};
	}

	if((**pp_host_t).rout_thread_were_canceled == 1)
	{
		(**pp_host_t).rout_thread_on_pause = 0;
		// Routing Thread creation
		if((ret = pthread_create(&(**pp_host_t).thread_routing, NULL, routing_thread, &(**pp_host_t))) != 0)  // I have doubt if the parameter is right
		{
			printf("ERROR: routing thread creation failed, ret = %d\n", ret);
			return -1;
		}	
	}

}

				//End of COMMUNICATION
/*************************************************************************************************************************************************************************************************************************************************************************/
/*************************************************************************************************************************************************************************************************************************************************************************/
				// The END ?


