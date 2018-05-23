# Justin-rn1host-version

This version should support multithreading. Yet, I have not protected all the shared memory from multiple access from different threads. All my new variables are containes in a structure (type : thread_structure) I called host_t. I pass this structure as a paramter to the communication, mapping,navigation and routing thread. 
The thread_managemen_before/after_cmd() functions are made to manage the threads when a command comes from the client.
