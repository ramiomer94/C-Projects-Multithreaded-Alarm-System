#include <stdio.h>        // Standard I/O functions for input/output
#include <stdlib.h>       // Standard library functions (e.g., malloc, free)
#include <pthread.h>      // Pthread library for multithreading
#include <semaphore.h>    // Semaphore library for synchronization
#include <string.h>       // String manipulation functions
#include <unistd.h>       // Unix standard functions (e.g., sleep)
#include <time.h>         // Time library for timestamps
#include "errors.h"       // Custom error-handling macros (not provided here)

#define MAX_MESSAGE_LENGTH 128 // Maximum allowable length for an alarm message
#define MAX_GROUPS 10000       // Maximum number of alarm groups supported

// Struct representing an alarm
typedef struct alarm_t {
    int alarm_id;                  // Unique ID for the alarm
    int group_id;                  // Group ID associated with the alarm
    int seconds;                   // Duration (in seconds) after which the alarm trigger
    char message[MAX_MESSAGE_LENGTH + 1]; // Message displayed by the alarm
    int isActive;                  // 1 if active, 0 if suspended
    int isChanged;                 // 1 if modified since creation
    int isCanceled;                // 1 if the alarm is canceled
    int isAssigned;                // 1 if the alarm is assigned to a display thread
    time_t created_at;             // Timestamp when the alarm was created
    time_t assigned_at;            // Timestamp when the alarm was assigned to a dsiplay thread
    time_t next_print_time;        // Time when the alarm is next due to print
    struct alarm_t *link;          // Pointer to the next alarm in the linked list
} alarm_t;

/* Global Variables */

// Synchronization primitives for multithreading
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER; // Condition variable for signaling changes
pthread_cond_t timed_wait = PTHREAD_COND_INITIALIZER; // Condition variable for triggering the timed wait
pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for condition variable protection
pthread_mutex_t request_mutex = PTHREAD_MUTEX_INITIALIZER; // Protects the `current_request_type` variable
pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for thread-level synchronization
sem_t rw_mutex;       // Semaphore for writer locks
sem_t mutex;          // Semaphore for reader count management
sem_t group_thread;   // Semaphore for synchronizing access to group_threads array

// Shared resources
int read_count = 0;                // Number of active readers (for reader/writer locks)
alarm_t *alarm_list = NULL;        // Head pointer for the linked list of alarms
static pthread_t group_threads[MAX_GROUPS] = {0}; // Thread IDs for group handling threads
char current_request_type[20] = ""; // Tracks the current request type (e.g., "Reactivate_Alarm")

/* Helper Functions */

// Function to retrieve the current system time in UNIX epoch format
time_t current_time() {
    return time(NULL); // Returns the current time as a `time_t` object
}

// Locks the alarm list for reading
void reader_lock() {
    int status;

    // Decrement the semaphore to acquire the mutex lock
    status = sem_wait(&mutex);
    if (status != 0)
        err_abort(status, "sem wait"); // Handles errors during semaphore wait

    read_count++; // Increment the reader count

    // If this is the first reader, lock the `rw_mutex` to prevent writers
    if (read_count == 1) {
        status = sem_wait(&rw_mutex);
        if (status != 0)
            err_abort(status, "sem wait"); // Handle errors during semaphore wait
    }

    // Release the mutex semaphore for other readers
    status = sem_post(&mutex);
    if (status != 0)
        err_abort(status, "sem post"); // Handle errors during semaphore post
}

// Unlocks the alarm list after reading
void reader_unlock() {
    int status;

    // Decrement the semaphore to acquire the mutex lock
    status = sem_wait(&mutex);
    if (status != 0)
        err_abort(status, "sem wait"); // Handle errors during semaphore wait

    read_count--; // Decrement the reader count

    // If this is the last reader, release the writer lock
    if (read_count == 0) {
        status = sem_post(&rw_mutex);
        if (status != 0)
            err_abort(status, "sem post"); // Handle errors during semaphore post
    }

    // Release the mutex semaphore for other readers
    status = sem_post(&mutex);
    if (status != 0)
        err_abort(status, "sem post"); // Handle errors during semaphore post
}

// Locks the alarm list for writing
void writer_lock() {
    int status;

    // Acquire the writer lock by decrementing `rw_mutex`
    status = sem_wait(&rw_mutex);
    if (status != 0)
        err_abort(status, "sem wait"); // Handle errors during semaphore wait
}

// Unlocks the alarm list after writing
void writer_unlock() {
    int status;

    // Release the writer lock by incrementing `rw_mutex`
    status = sem_post(&rw_mutex);
    if (status != 0)
        err_abort(status, "sem post"); // Handle errors during semaphore post
}


/*
Function: start_alarm
Summary: Adds a new alarm to the linked list. The alarm is sorted by alarm_id. If the alarm_id already exists,
 it returns an error. It also ensures thread safety during the modification.

Request Purpose: The Start_Alarm request creates a new alarm with a unique ID, assigns it to a group, sets its duration,
 and stores the message. This alarm will later trigger based on its settings.
 */

void start_alarm(int alarm_id, int group_id, int seconds, char *message) {
    alarm_t **prev, *current; // Pointers to traverse and insert the alarm
    int status;

    writer_lock(); // Ensures only one writer modifies the alarm list at a time

    // Traverse the list to ensure no duplicate alarm_id exists
    current = alarm_list;
    while (current != NULL) {
        if (current->alarm_id == alarm_id) { // Check for duplicate alarm_id
            fprintf(stderr, "Error: Alarm with ID %d already exists.\n", alarm_id);
            writer_unlock();
            return; // Exit if duplicate is found
        }
        current = current->link; // Move to the next alarm
    }

    // Allocate memory for the new alarm
    alarm_t *alarm = (alarm_t *)malloc(sizeof(alarm_t));
    if (alarm == NULL) // Handle memory allocation failure
        errno_abort("Allocate alarm");

    // Initialize the new alarm's attributes
    alarm->alarm_id = alarm_id;
    alarm->group_id = group_id;
    alarm->seconds = seconds;
    strncpy(alarm->message, message, MAX_MESSAGE_LENGTH + 1); // Copy message
    alarm->created_at = current_time(); // Timestamp when the alarm was added
    alarm->assigned_at = 0;             // Alarm has not been assigned yet (default value 0)
    alarm->next_print_time = alarm->created_at + seconds; // Set the initial next print time
    alarm->isActive = 1; // The alarm is active by default
    alarm->isChanged = 0; // No changes made yet
    alarm->isCanceled = 0; // The alarm is not canceled
    alarm->isAssigned = 0; // The alarm is not assigned to a display thread
    alarm->link = NULL; // End of the linked list

    // Insert the alarm into the sorted linked list by alarm_id
    prev = &alarm_list; // Start from the head of the list
    current = alarm_list;
    while (current != NULL && current->alarm_id < alarm_id) {
        prev = &current->link; // Move to the next position
        current = current->link;
    }

    // Insert the new alarm into the linked list
    *prev = alarm; // Point the previous node to the new alarm
    alarm->link = current; // Point the new alarm to the next node

    // Print details of the new alarm
    printf("Alarm(<%d>) Inserted by Main Thread <%ld> Into Alarm List at <%ld>: "
           "Group(<%d>) <%d %s>\n",
           alarm->alarm_id, pthread_self(), alarm->created_at,
           alarm->group_id, alarm->seconds, alarm->message);

    writer_unlock(); // Unlock the writer lock

    // Signal any threads waiting for alarm changes
    status = pthread_cond_signal(&alarm_cond);
    if (status != 0)
        err_abort(status, "Cond Signal");
}

/*
Function: change_alarm
Summary: Modifies the attributes of an existing alarm (e.g., group_id, duration, or message). 
If the alarm does not exist, it returns an error.

Request Purpose: The Change_Alarm request is used to update an alarm’s details, such as its group,
 duration, or message, without deleting or recreating it.
*/

void change_alarm(int alarm_id, int group_id, int seconds, char *message) {
    alarm_t *current; // Pointer to traverse the list
    int isChanged = 0; //Flag to check if a valid change was made
    int status;

    writer_lock(); // Ensures exclusive access to the alarm list

    // Traverse the list to find the alarm with the specified alarm_id
    current = alarm_list;
    while (current != NULL && current->alarm_id != alarm_id) {
        current = current->link; // Move to the next alarm
    }

    // If the alarm is found, update its properties
    if (current != NULL) {
        current->group_id = group_id; // Update the group ID
        current->seconds = seconds; // Update the duration
        strncpy(current->message, message, MAX_MESSAGE_LENGTH + 1); // Update the message
        current->isAssigned = 0; // Mark the alarm as unassgined to a display thread
        current->isChanged = 1; // Mark the alarm as changed
        isChanged = 1; // Set the flag to indicate a successful change
        

        // Print details of the updated alarm
        printf("Alarm(<%d>) Changed at <%ld>: Group(<%d>) <%d %s>\n",
               current->alarm_id, current_time(), current->group_id, current->seconds, current->message);
    } 

     writer_unlock(); // Release the writer lock

    if (!isChanged) { 
        // If the alarm with the specified ID was not found
        fprintf(stderr, "Error: Alarm ID %d not found\n", alarm_id);
    }

    if (isChanged) {
        // Notify all waiting threads about the change in the alarm list
        int status = pthread_cond_broadcast(&alarm_cond);
        if (status != 0)
            err_abort(status, "Signal cond"); // Handle potential errors in signaling
    }
}

/*
Function: cancel_alarm
Summary: Cancels an existing alarm by removing it from the linked list. 
It also sets the isCanceled flag so threads handling the alarm can act accordingly.

Request Purpose: The Cancel_Alarm request removes an alarm completely from the list,
 effectively deactivating it and freeing its allocated memory.
*/

void cancel_alarm(int alarm_id) {
    int cancelled_alarm_id, cancelled_seconds;
    char cancelled_message[MAX_MESSAGE_LENGTH + 1];
    alarm_t **prev, *current; // Pointers for traversal and removal

    writer_lock(); // Ensures exclusive access to the alarm list

    // Traverse the list to find the alarm with the specified alarm_id
    prev = &alarm_list; // Start from the head of the list
    current = alarm_list;
    while (current != NULL && current->alarm_id != alarm_id) {
        prev = &current->link; // Move to the next alarm
        current = current->link;
    }

    // If the alarm is found, cancel it
    if (current != NULL) {
        current->isCanceled = 1; // Mark the alarm as canceled
        cancelled_alarm_id = current->alarm_id;
        cancelled_seconds = current->seconds;
        strncpy(cancelled_message, current->message, MAX_MESSAGE_LENGTH + 1);

        // Remove the alarm from the linked list
        *prev = current->link; // Bypass the current node
        free(current); // Free the memory allocated for the alarm

        // Print details of the canceled alarm
        printf("Alarm(<%d>) Canceled at <%ld>: <%d %s>\n",
               cancelled_alarm_id, current_time(), cancelled_seconds, cancelled_message);
    } else {
        // Alarm with the specified ID was not found
        fprintf(stderr, "Error: Alarm ID %d not found\n", alarm_id);
    }

    writer_unlock(); // Release the writer lock
}

/*
Function: suspend_alarm
Summary: Temporarily deactivates an alarm by setting its isActive flag to 0.

Request Purpose: The Suspend_Alarm request is used to temporarily pause an alarm without deleting it.
 Suspended alarms can be reactivated later.
*/

void suspend_alarm(int alarm_id) {
    alarm_t *current; // Pointer to traverse the linked list of alarms

    writer_lock(); // Ensures exclusive access to the `alarm_list` to avoid race conditions

    // Traverse the `alarm_list` to find the alarm with the specified `alarm_id`
    current = alarm_list;
    while (current != NULL) {
        if (current->alarm_id == alarm_id) { // Check if the current alarm matches the ID
            current->isActive = 0; // Suspend the alarm by marking it as inactive
            printf("Alarm(<%d>) Suspended at <%ld>: Group(<%d>) <%d %s>\n",
                   current->alarm_id, // Alarm ID
                   current_time(),    // Current system time
                   current->group_id, // Group ID of the alarm
                   current->seconds,  // Duration (in seconds)
                   current->message); // Alarm message
            writer_unlock(); // Release the writer lock after handling the alarm
            return; // Exit the function once the target alarm is processed
        }
        current = current->link; // Move to the next alarm in the list
    }

    // If no alarm with the specified ID is found, print an error message
    fprintf(stderr, "Error: Alarm ID %d not found.\n", alarm_id);
    writer_unlock(); // Release the writer lock before exiting
}

/*
Function: reactivate_alarm
Summary: Reactivates a suspended alarm by setting its isActive flag to 1.

Request Purpose: The Reactivate_Alarm request resumes a previously suspended alarm, 
allowing it to trigger normally.
*/

void reactivate_alarm(int alarm_id) {
    alarm_t *current; // Pointer to traverse the linked list of alarms

    writer_lock(); // Ensures exclusive access to the `alarm_list` to avoid race conditions

    // Traverse the alarm list to find the alarm with the specified `alarm_id`
    current = alarm_list;
    while (current != NULL) {
        if (current->alarm_id == alarm_id) { // Check if the current alarm matches the ID
            if (current->isActive == 0) { // Only proceed if the alarm is suspended (inactive)
                current->isActive = 1; // Reactivate the alarm by marking it as active
                printf("Alarm(<%d>) Reactivated at <%ld>: Group(<%d>) <%d %s>\n",
                       current->alarm_id, // Alarm ID
                       current_time(),    // Current system time
                       current->group_id, // Group ID of the alarm
                       current->seconds,  // Duration (in seconds)
                       current->message); // Alarm message
            } else {
                // If the alarm is already active, print an error message
                fprintf(stderr, "Error: Alarm ID %d is already active.\n", alarm_id);
            }
            writer_unlock(); // Release the writer lock after handling the alarm
            return; // Exit the function once the target alarm is processed
        }
        current = current->link; // Move to the next alarm in the list
    }

    // If no alarm with the specified ID is found, print an error message
    fprintf(stderr, "Error: Alarm ID %d not found.\n", alarm_id);
    writer_unlock(); // Release the writer lock before exiting
}

/*
Function: view_alarms
Summary:
The view_alarms function displays all active and suspended alarms grouped by their group_id.
It uses a reader lock to ensure safe concurrent access to the shared alarm_list. 
This function prints the details of each alarm (e.g., alarm_id, creation time, group ID, status) 
along with the thread assigned to handle alarms in that group.

Purpose:
The View_Alarms request is designed to provide a snapshot of the current state of all alarms in the system.
It helps monitor active alarms grouped by their associated group IDs, offering a clear view of the system’s state 
at a given moment.
*/

void view_alarms() {
    reader_lock(); // Lock for reading to ensure no writers modify the shared `alarm_list` during traversal

    time_t view_time = current_time(); // Fetch the current system time
    int counter = 0; // Counter to enumerate group threads
    printf("View Alarms at <%ld>:\n", view_time); // Print the time when alarms are viewed

    // Iterate through all potential group IDs
    for (int group_id = 0; group_id < MAX_GROUPS; group_id++) {
        if (group_threads[group_id] != 0) { // Check if there is a thread handling this group
            // Print the thread ID assigned to this group
            printf("%d. Display Thread <%ld> Assigned:\n", counter + 1, group_threads[group_id]);

            alarm_t *current = alarm_list; // Start traversing the alarm list
            int alarm_index = 0; // Initialize an index for alarms within the current group

            // Traverse the alarm list to find alarms belonging to this group
            while (current != NULL) {
                if (current->group_id == group_id) { // Check if the alarm belongs to the current group
                    // Print the details of the alarm
                    printf("   %d%c. Alarm(<%d>): Created at <%ld> Assigned at <%ld> <%d %s> Status <%s>\n",
                           counter + 1,                 // Group-level counter
                           'a' + alarm_index,          // Sub-index (a, b, c, etc.) for alarms within the group
                           current->alarm_id,          // Unique alarm ID
                           current->created_at,        // Time when the alarm was created
                           current-> assigned_at,      // Time when the alarm was assigned
                           current->seconds,           // Alarm duration (in seconds)
                           current->message,           // Alarm message
                           current->isActive ? "Active" : "Suspended"); // Status of the alarm (Active/Suspended)

                    alarm_index++; // Increment the index for alarms in this group
                }
                current = current->link; // Move to the next alarm in the list
            }
            counter++; // Increment the counter for groups
        }
    }

    reader_unlock(); // Unlock the reader lock to allow other threads access to the `alarm_list`
}

/*
Thread start routine: display_alarm_threads
Summary:
The display_alarm_threads function is a thread routine that monitors and manages alarms
for a specific group (identified by group_id). It collects active alarms for the group,
waits for the appropriate trigger time, and then prints the alarm message or handles its 
cancellation, suspension, or reassignment. The function continuously runs, 
updating the status of alarms in its group.

Purpose:
This routine is executed by a dedicated thread for each alarm group. 
Its primary role is to ensure that active alarms in the group are processed at their respective trigger times,
 and their statuses (active, canceled, or changed) are appropriately handled.
*/
void *display_alarm_threads(void *group_ID) {
    int status;
    int group_id = *(int *)group_ID; // Extract the group ID passed as an argument
    free(group_ID);  // Free dynamically allocated memory for the group ID
    struct timespec timeout; // Struct to store the timeout for conditional waits
    pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for thread-level synchronization

    while (1) { // Continuous monitoring loop for this group
        reader_lock(); // Lock the shared alarm list for reading
        alarm_t *current = alarm_list; // Start from the head of the alarm list
        alarm_t *group_alarms[MAX_GROUPS] = {NULL};  // Array to store active alarms for this group
        int alarm_count = 0; // Counter for active alarms in the group

        // Collect all active alarms for the current group
        while (current != NULL) {
            if (current->group_id == group_id && current->isActive && !current->isCanceled) {
                // Initialize next_print_time if not already set
                if (current->next_print_time == 0) {
                    current->next_print_time = current_time() + current->seconds;
                }

                group_alarms[alarm_count++] = current; // Add the alarm to the group array
            }
            current = current->link; // Move to the next alarm in the list
        }
        reader_unlock(); // Unlock the reader lock after collecting alarms

        // Process each alarm in the group
        for (int i = 0; i < alarm_count; i++) {
            time_t now = current_time(); // Get the current time

            // Skip processing if the alarm's next print time hasn't arrived
            if (group_alarms[i]->next_print_time > now) {
                continue;
            }

            // Lock the thread mutex
            status = pthread_mutex_lock(&thread_mutex);
            if (status != 0)
                err_abort(status, "Lock mutex");

            // Wait until the next print time if necessary
            if (group_alarms[i]->next_print_time > now) {
                clock_gettime(CLOCK_REALTIME, &timeout);
                timeout.tv_sec = group_alarms[i]->next_print_time;
                timeout.tv_nsec = 0;

                // Wait until the next print time or a condition variable signal
                pthread_cond_timedwait(&timed_wait, &thread_mutex, &timeout);
            }

            // Print the alarm and update the next print time
            now = current_time(); // Refresh the current time
            if (group_alarms[i]->next_print_time <= now) {
                reader_lock(); // Lock the shared list to recheck the alarm's status
                current = alarm_list; // Reset to the start of the alarm list
                int alarm_found = 0; // Flag to check if the alarm is still valid

                // Traverse the alarm list to recheck the alarm's status
                while (current != NULL) {
                    if (current->alarm_id == group_alarms[i]->alarm_id) {
                        alarm_found = 1; // Alarm found in the list

                        if (current->isCanceled) { // If the alarm is canceled
                            printf("Display Thread <%ld> Has Stopped Printing Message of Alarm(<%d>) "
                                   "at <%ld>: Group(<%d>) <%d %s>\n",
                                   pthread_self(), current->alarm_id, current_time(),
                                   group_id, current->seconds, current->message);
                        } else if (current->group_id != group_id) { // If the alarm's group has changed
                            printf("Display Thread <%ld> Has Stopped Printing Message of Alarm(<%d>) "
                                   "at <%ld>: Changed Group(<%d>) <%d %s>\n",
                                   pthread_self(), current->alarm_id, current_time(),
                                   current->group_id, current->seconds, current->message);
                        } else if (current->isChanged) { // If the alarm's properties were modified
                            printf("Display Thread <%ld> Starts to Print Changed Message Alarm(<%d>) "
                                   "at <%ld>: Group(<%d>) <%d %s>\n",
                                   pthread_self(), current->alarm_id, current_time(),
                                   group_id, current->seconds, current->message);
                            current->isChanged = 0;  // Reset the change flag
                        } else if (current->isActive) { // If the alarm is still active
                            printf("Alarm(<%d>) Printed by Display Alarm Thread <%ld> at <%ld>: "
                                   "Group(<%d>) <%d %s>\n",
                                   current->alarm_id, pthread_self(), current_time(),
                                   group_id, current->seconds, current->message);

                            // Update the next print time for this alarm
                            current->next_print_time = now + current->seconds;
                        }
                        break; // Stop checking further alarms
                    }
                    current = current->link; // Move to the next alarm in the list
                }

                if (!alarm_found) { // If the alarm is no longer in the list
                    printf("Display Thread <%ld> Has Stopped Printing Message of Alarm(<%d>) "
                           "at <%ld>: Group(<%d>) <%d %s>\n",
                           pthread_self(), group_alarms[i]->alarm_id, current_time(),
                           group_id, group_alarms[i]->seconds, group_alarms[i]->message);
                }
                reader_unlock(); // Unlock the reader lock
            }

            status = pthread_mutex_unlock(&thread_mutex); // Unlock the thread mutex
            if (status != 0)
                err_abort(status, "Unlock mutex");
        }

        // Sleep briefly if no alarms are found to avoid busy waiting
        if (alarm_count == 0) {
            usleep(100000); // Sleep for 100 milliseconds
        }
    }

    return NULL; // Thread exit point (should never reach here)
}
/*
Thread start routine: alarmGroup__displayCreation_thread
Summary:
This function is a thread routine responsible for managing the creation
and assignment of display threads for alarm groups. 
It monitors the alarm_list for changes and ensures that each alarm group
has a dedicated display thread to handle its alarms. 
If an alarm is added to a group without an existing thread,
a new thread is created for that group. The function skips processing
 for certain requests like Suspend_Alarm and Reactivate_Alarm.

Purpose:
The alarmGroup__displayCreation_thread is critical for ensuring that each alarm group is assigned a thread
to manage its alarms. It dynamically creates threads as needed and assigns alarms to
the correct group thread while avoiding redundant thread creation.
*/

void *alarmGroup__displayCreation_thread(void *arg) {
    int status; // Status variable for error checking
    int *group_id_ptr; // Pointer to dynamically allocate memory for group IDs
    pthread_t new_thread; // Variable to store the ID of newly created threads

    while (1) { // Infinite loop to continuously monitor alarm list changes
        // Wait for a signal indicating changes to the alarm list
        status = pthread_cond_wait(&alarm_cond, &wait_mutex); // Wait for condition signal
        if (status != 0)
            err_abort(status, "Wait on cond"); // Handle errors during conditional wait

        reader_lock(); // Lock the alarm list for reading to ensure thread-safe traversal
        alarm_t *current = alarm_list; // Start at the head of the linked list of alarms

        // Traverse the alarm list to handle each unassigned alarm
        while (current != NULL) {
            int group_id = current->group_id; // Extract the group ID of the current alarm

            // Check if the alarm is not yet assigned to a display thread
            if (!current->isAssigned) {
                // Lock the group_threads array to manage thread assignments
                status = sem_wait(&group_thread);
                if (status != 0)
                    err_abort(status, "Sem wait"); // Handle errors during semaphore wait
                
                current->assigned_at = current_time(); // Time when the alarm is assigned to a display thread

                // If no thread exists for the group, create a new display thread
                if (group_threads[group_id] == 0) {
                    group_id_ptr = (int *)malloc(sizeof(int)); // Allocate memory for the group ID
                    if (group_id_ptr == NULL)
                        errno_abort("Allocate group ID"); // Handle memory allocation errors

                    *group_id_ptr = group_id; // Set the allocated group ID

                    // Create a new thread to handle alarms for this group
                    status = pthread_create(&new_thread, NULL, display_alarm_threads, group_id_ptr);
                    if (status != 0)
                        err_abort(status, "Create Display Thread"); // Handle thread creation errors

                    // Assign the new thread to the group
                    group_threads[group_id] = new_thread; 
                    printf("Alarm Group Display Creation Thread Created New Display Alarm Thread <%ld> "
                           "For Alarm(<%d>) at <%ld>: Group(<%d>) <%d %s>\n",
                           new_thread,            // Thread ID
                           current->alarm_id,     // Alarm ID
                           current->assigned_at,  // Current system time (Timestamp when the alarm was assigned to a display thread)
                           group_id,              // Group ID
                           current->seconds,      // Alarm duration
                           current->message);     // Alarm message
                } else {
                    // If a thread already exists for the group, log the assignment
                    printf("Alarm Thread Display Alarm Thread <%ld> Assigned to Display Alarm(<%d>) "
                           "at <%ld>: Group(<%d>) <%d %s>\n",
                           group_threads[group_id], // Existing thread ID
                           current->alarm_id,       // Alarm ID
                           current->assigned_at,    // Current system time 
                           group_id,                // Group ID
                           current->seconds,        // Alarm duration
                           current->message);       // Alarm message
                }

                // Mark the current alarm as assigned to avoid redundant assignments
                current->isAssigned = 1;

                // Unlock the group_threads array to allow access by other threads
                status = sem_post(&group_thread);
                if (status != 0)
                    err_abort(status, "Sem post"); // Handle errors during semaphore post
            }

            current = current->link; // Move to the next alarm in the list
        }

        reader_unlock(); // Unlock the alarm list after traversal
        usleep(100000); // Introduce a small delay to avoid busy-waiting
    }

    return NULL; // Exit point for the thread (should not be reached in the infinite loop)
}

/*
Thread start routine: alarmGroup_displayRemoval_thread
Summary:
This function is a monitoring thread responsible for identifying and terminating
display threads for alarm groups that no longer have active alarms. 
It continuously checks the alarm_list and dynamically cancels threads assigned to groups
that no longer contain alarms. This ensures efficient resource usage by preventing unnecessary threads from running.

Purpose:
The alarmGroup_displayRemoval_thread maintains system efficiency by removing idle threads.
When a group has no active alarms, the thread assigned to that group is terminated,
and its entry in the group_threads array is cleared.
*/
void *alarmGroup_displayRemoval_thread(void *arg) {
    int status; 
    while (1) { // Infinite loop to continuously monitor the alarm list
        // Wait for a signal indicating changes in the alarm list
        status = pthread_cond_wait(&alarm_cond, &wait_mutex);
        if (status != 0)
            err_abort(status, "Wait cond");

        // Lock the group_threads array for exclusive access
        status = sem_wait(&group_thread);
        if (status != 0)
            err_abort(status, "Sem wait");

        // Iterate through all potential group IDs
        for (int group_id = 0; group_id < MAX_GROUPS; group_id++) {
            if (group_threads[group_id] != 0) { // Check if a thread exists for this group
                int alarms_exist = 0; // Flag to indicate whether alarms exist in this group

                reader_lock(); // Lock the shared `alarm_list` for reading
                alarm_t *current = alarm_list; // Start at the head of the alarm list

                // Traverse the alarm list to check for alarms belonging to this group
                while (current != NULL) {
                    if (current->group_id == group_id) { // Check if the alarm belongs to this group
                        alarms_exist = 1; // Set the flag if at least one alarm exists
                        break; // Exit the loop as no further checks are needed
                    }
                    current = current->link; // Move to the next alarm in the list
                }

                reader_unlock(); // Unlock the shared `alarm_list`

                if (!alarms_exist) { // If no alarms exist for this group
                    // Terminate the thread for this group
                    status = pthread_cancel(group_threads[group_id]);
                    if (status != 0)
                        err_abort(status, "Terminate Display Thread");

                    // Print a message indicating thread termination
                    printf("No More Alarms in Group(<%d>) Alarm Removal Thread Has Removed "
                           "Display Alarm Thread <%ld> at <%ld>: Group(<%d>)\n",
                           group_id, group_threads[group_id], current_time(), group_id);

                    group_threads[group_id] = 0; // Clear the thread ID for this group
                }
            }
        }

        // Unlock the group_threads array
        status = sem_post(&group_thread);
        if (status != 0)
            err_abort(status, "Sem post");

        // Introduce a small sleep to prevent busy-waiting
        usleep(100000);
    }

    return NULL; // Exit point for the thread (unreachable in the infinite loop)
}

/*
Thread: main
Summary:
The main function acts as the central control for the alarm management system.
It initializes resources such as semaphores and threads, continuously processes user commands,
and handles cleanup on program termination. The function provides an interactive console for users
to input commands to create, modify, or manage alarms. It coordinates the creation and removal
of threads for alarm display and ensures synchronization and thread safety throughout the program.

Purpose:
This function implements a command-driven alarm system, allowing users to interact with alarms via textual commands.
It ensures proper management of threads, shared resources, and synchronization mechanisms.
*/
int main() {
    int status; // Status variable to capture the return codes of system calls
    char line[256]; // Buffer to store user input commands
    char message[MAX_MESSAGE_LENGTH + 1]; // Buffer for alarm message input
    int group_id, alarm_id, seconds; // Variables to hold parsed command inputs
    pthread_t display_thread_creation, display_thread_removal; // Threads for alarm creation and removal monitoring

    // Initialize semaphores
    status = sem_init(&mutex, 0, 1); // Protects the `read_count` variable used in reader-writer synchronization
    if (status != 0)
        err_abort(status, "Initialize Semaphore"); // Handle errors during semaphore initialization

    status = sem_init(&rw_mutex, 0, 1); // Synchronizes reader and writer access to shared resources
    if (status != 0)
        err_abort(status, "Initialize Semaphore");

    status = sem_init(&group_thread, 0, 1); // Synchronizes access to the `group_threads` array
    if (status != 0)
        err_abort(status, "Initialize Semaphore");

    // Create a thread to handle alarm group display thread creation
    status = pthread_create(&display_thread_creation, NULL, alarmGroup__displayCreation_thread, NULL);
    if (status != 0)
        err_abort(status, "Create Display Creation Thread"); // Handle errors during thread creation

    // Create a thread to handle alarm group display thread removal
    status = pthread_create(&display_thread_removal, NULL, alarmGroup_displayRemoval_thread, NULL);
    if (status != 0)
        err_abort(status, "Create Display Removal Thread");

    // Main loop to process user commands
    while (1) {
        printf("Alarm> "); // Prompt for user input
        if (fgets(line, sizeof(line), stdin) == NULL) exit(0); // Read user input; exit on EOF
        if (strlen(line) <= 1) continue; // Skip empty commands

        // Parse and process user commands
        if (strncmp(line, "Start_Alarm", 11) == 0) {
            // Parse "Start_Alarm" command
            if (sscanf(line, "Start_Alarm(%d): Group(%d) %d %128[^\n]", &alarm_id, &group_id, &seconds, message) < 4) {
                fprintf(stderr, "Bad Command\n"); // Invalid format
            } else if (alarm_id <= 0 || group_id <= 0) {
                fprintf(stderr, "Bad Command\n"); // Invalid IDs
            } else {
                start_alarm(alarm_id, group_id, seconds, message); // Add the new alarm
                status = pthread_cond_broadcast(&alarm_cond); // Notify waiting threads of alarm changes
                if (status != 0)
                    err_abort(status, "Signal cond"); // Handle signaling errors
            }
        } else if (strncmp(line, "Change_Alarm", 12) == 0) {
            // Parse "Change_Alarm" command
            if (sscanf(line, "Change_Alarm(%d): Group(%d) %d %128[^\n]", &alarm_id, &group_id, &seconds, message) < 4) {
                fprintf(stderr, "Bad Command\n"); // Invalid format
            } else if (alarm_id <= 0 || group_id <= 0) {
                fprintf(stderr, "Bad Command\n"); // Invalid IDs
            } else {
                change_alarm(alarm_id, group_id, seconds, message); // Modify the alarm
            }
        } else if (strncmp(line, "Cancel_Alarm", 12) == 0) {
            // Parse "Cancel_Alarm" command
            if (sscanf(line, "Cancel_Alarm(%d)", &alarm_id) < 1) {
                fprintf(stderr, "Bad Command\n"); // Invalid format
            } else if (alarm_id <= 0) {
                fprintf(stderr, "Bad Command\n"); // Invalid ID
            } else {
                cancel_alarm(alarm_id); // Cancel the alarm
                status = pthread_cond_broadcast(&alarm_cond); // Notify waiting threads of alarm cancellation
                if (status != 0)
                    err_abort(status, "Signal cond");
            }
        } else if (strncmp(line, "Suspend_Alarm", 13) == 0) {
            // Parse "Suspend_Alarm" command
            if (sscanf(line, "Suspend_Alarm(%d)", &alarm_id) < 1) {
                fprintf(stderr, "Bad Command\n"); // Invalid format
            } else if (alarm_id <= 0) {
                fprintf(stderr, "Bad Command\n"); // Invalid ID
            } else {
                // Update the current request type for synchronization
                status = pthread_mutex_lock(&request_mutex);
                if (status != 0)
                    err_abort(status, "Request mutex lock");
                strncpy(current_request_type, "Suspend_Alarm", sizeof(current_request_type)); // Set request type
                status = pthread_mutex_unlock(&request_mutex);
                if (status != 0)
                    err_abort(status, "Request mutex unlock");
                suspend_alarm(alarm_id); // Suspend the alarm
                status = pthread_cond_broadcast(&alarm_cond); // Notify waiting threads of suspension
                if (status != 0)
                    err_abort(status, "Signal cond");
            }
        } else if (strncmp(line, "Reactivate_Alarm", 16) == 0) {
            // Parse "Reactivate_Alarm" command
            if (sscanf(line, "Reactivate_Alarm(%d)", &alarm_id) < 1) {
                fprintf(stderr, "Bad Command\n"); // Invalid format
            } else if (alarm_id <= 0) {
                fprintf(stderr, "Bad Command\n"); // Invalid ID
            } else {
                // Update the current request type for synchronization
                status = pthread_mutex_lock(&request_mutex);
                if (status != 0)
                    err_abort(status, "Request mutex lock");
                strncpy(current_request_type, "Reactivate_Alarm", sizeof(current_request_type)); // Set request type
                status = pthread_mutex_unlock(&request_mutex);
                if (status != 0)
                    err_abort(status, "Request mutex unlock");
                reactivate_alarm(alarm_id); // Reactivate the alarm
                status = pthread_cond_broadcast(&alarm_cond); // Notify waiting threads of reactivation
                if (status != 0)
                    err_abort(status, "Signal cond");
            }
        } else if (strncmp(line, "View_Alarms", 11) == 0) {
            view_alarms(); // Display all alarms in the system
        } else {
            printf("Error: Invalid request\n"); // Handle unrecognized commands
        }
    }

    // Wait for threads to terminate (unreachable due to infinite loop above)
    status = pthread_join(display_thread_creation, NULL); // Join the creation thread
    if (status != 0)
        err_abort(status, "Terminate Display Creation Thread");

    status = pthread_join(display_thread_removal, NULL); // Join the removal thread
    if (status != 0)
        err_abort(status, "Terminate Display Removal Thread");

    // Clean up semaphores
    status = sem_destroy(&rw_mutex);
    if (status != 0)
        err_abort(status, "Destroy Semaphore");

    status = sem_destroy(&mutex);
    if (status != 0)
        err_abort(status, "Destroy Semaphore");

    status = sem_destroy(&group_thread);
    if (status != 0)
        err_abort(status, "Destroy Semaphore");

    return 0; // Exit the program
}