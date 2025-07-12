#include <stdio.h> // Standard I/O
#include <stdlib.h> // Memory allocation
#include <string.h> // String manipulation
#include <pthread.h> // Thread creation and management
#include <time.h> // Time management especially for using the UNIX Epoch timing standard
#include <stdbool.h> // For using boolean values. Used them instead of flags in the view_alarm() function
#include "errors.h" // custom error-handling header provided by the professor in the additional materials of assignment 2
#include <sys/select.h> // For select()

#define MAX_MESSAGE_LENGTH 128 //Maximum length of an alarm message
#define MAX_TYPE_LENGTH 4 // Maximum length of the alarm type. We chose it to be T followed by 3 digits between 0-999
#define MAX_ALARMS_PER_THREAD 2 // Maximum number of alarms per display thread.
#define MAX_NUMBER_OF_DISPLAY_THREADS 100 // Maximum number of unique display threads.

// Defines the alarm_t struct to represent each alarm.
typedef struct alarm_t {
    int alarm_id; // Unique identifier for each alarm. We designed our code so that no two alarms can have the same alarm_id
    char type[MAX_TYPE_LENGTH + 1]; // Type of the alarm (e.g., "T001" or "T200", or "T2")
    int seconds; // Duration in seconds after which the alarm will trigger/expire
    char message[MAX_MESSAGE_LENGTH + 1]; // Message to display when the alarm is periodically printed by the display thread
    time_t insert_time; // Times in UNIX EPOCH seconds when the alarm was added
    int is_type_changed; // Flag to indicate if the alarm's type has changed
    int is_canceled; // Flag to indicate if the alarm has been canceled
    int is_expired; // Flag to indicate if the alarm has expired
    pthread_t display_thread_id; // Display thread ID responsible for this alarm
    struct alarm_t *link; // Pointer to the next alarm in the linked list
} alarm_t;


// Global alarm list and synchronization tools
alarm_t *alarm_list = NULL; // Head of the linked list of alarms initialized to NULL (i.e., the list is empty)
pthread_mutex_t alarm_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex for alarm list synchronization
pthread_cond_t alarm_cond = PTHREAD_COND_INITIALIZER;  // For the alarm thread to signal changes
pthread_cond_t display_cond = PTHREAD_COND_INITIALIZER; // For display threads to wait periodically

/* Helper function to retriev the current system time expressed as the number of seconds 
from the UnixEpoch Jan 1 1970 00:00; */
time_t current_time() {
    return time(NULL); // Returns the current time as a `time_t` object
}
/*Add a new alarm to the alarm list, ensuring unique alarm_id in which no two alarms in the alarm list would have the 
same alarm_id .
Parameters:
	-   alarm_id: Unique identifier for the alarm.
	-   type: String representing the type/category of the alarm (e.g., “T001” or "T900").
	-   seconds: Duration in seconds after which the alarm should trigger/expire and stop printing the alarm message
	-   message: Custom message for the alarm.
*/
void insert_alarm(int alarm_id, char *type, int seconds, char *message) {
    int status; // Holds the return status for error-checking during mutex lock/unlock and condition signaling.
    alarm_t **prev, *current; // Pointers to traverse and insert the alarm within alarm_list.

    // Lock mutex to safely access alarm_list and to ensure that only one thread can modify alarm_list at a time.
    status = pthread_mutex_lock(&alarm_mutex);
    // If locking fails, the err_abort function is called. It prints an error and terminates the program.
    if (status != 0)
        err_abort(status, "Lock mutex");

    /* Check if an alarm with the same alarm_id already exists.This is done to ensure that each alarm 
    added to alarm_list has a unique alarm_id.*/

    // current is set to the head of alarm_list.
    current = alarm_list;

    // Traverse alarm_list to see if any existing alarm has the same alarm_id.
    // The while loop iterates through each alarm in the list
    while (current != NULL) {
        if (current->alarm_id == alarm_id) { // Alarm with the same ID exists, print error and return
            printf("Error: Alarm with ID %d already exists.\n", alarm_id);
            // The mutex is unlocked to allow other threads to access alarm_list.
            status = pthread_mutex_unlock(&alarm_mutex);
            //  If unlocking the mutex fails, err_abort is called.
            if (status != 0)
                err_abort(status, "Unlock mutex");
            return; // The function returns without inserting the new alarm.
        }
        current = current->link; // Move the pointer to the next alarm structure in the alarm list
    }

    // Allocate memory for a new alarm and initialize it
    alarm_t *alarm = (alarm_t *)malloc(sizeof(alarm_t));
    //If memory allocation fails, errno_abort handles the error. It prints the assigned message and terminates the program.
    if (alarm == NULL)
        errno_abort("Allocate alarm");

    // alarm_id, type, seconds, and message are set to the values passed to the function.
    alarm->alarm_id = alarm_id;
    strcpy(alarm->type, type);
    alarm->seconds = seconds;
    strncpy(alarm->message, message, MAX_MESSAGE_LENGTH + 1);
    alarm->insert_time = current_time(); //insert_time is set to the current time, recording when the alarm was added.
    alarm->is_type_changed = 0; //Initialized to 0 (false), indicating no changes has occured to the fields of the alarm
    alarm->is_canceled = 0; // Initialized to 0 (false), indicating no cancellation has occured
    alarm->is_expired = 0; // Initialized to 0 (false), indicating the alarm has not expired
    alarm->display_thread_id = 0; // display_thread_id is set to 0, as the alarm hasn’t been assigned to a display thread.
    alarm->link = NULL; // link is set to NULL to denote the end of the list.

    // Insert the new alarm into the sorted list by alarm_id. Eensures the alarm_list is sortedin ascending order of alarm_id.
    prev = &alarm_list; // Initialized to point to the header of the alarm_list
    current = alarm_list; // Initialized to point to the first alarm_t structure in the alarm_list

    /*The while loop advances prev and current until it reaches the correct position where 
    the new alarm should be inserted (i.e., where current has a higher alarm_id). */
    while (current != NULL && current->alarm_id < alarm_id) {
        prev = &current->link; 
        current = current->link;
    }
    // The new alarm is inserted her:
    *prev = alarm; // The previous alarm’s link is set to the new alarm.
    alarm->link = current; // The new alarm’s link points to current, maintaining the linked list structure

    /* Prints a message showing the new alarm’s alarm_id, the ID of the thread that added it, the timestamp, 
    the new alarm's type, duration, and custom message */
    printf("Alarm(%d) Inserted by Main Thread (%ld) Into Alarm List at %ld: %s %d %s\n",
           alarm_id, pthread_self(), alarm->insert_time, alarm->type, alarm->seconds, alarm->message);

    // Unlocks the mutex to allow other threads to access alarm_list.
    status = pthread_mutex_unlock(&alarm_mutex);
    //  If unlocking the mutex fails, err_abort is called.
    if (status != 0)
        err_abort(status, "Unlock mutex");
}


/* The change_alarm function is used to update the properties of an existing alarm by its unique alarm_id.
The function locates the alarm by alarm_id in alarm_list. Then it updates the alarm’s type, seconds, and message.
It sets a flag to indicate the alarm type has changed and after that it signal the alarm thread about the  change
in the alarms' traits.
Parameters:
	-   alarm_id: Unique identifier for the alarm.
    -   type: String representing the "new" type/category of the alarm (e.g., “T001” or "T900").
	-   seconds: Duration in "new" seconds after which the alarm should trigger/expire and stop printing the alarm message
	-   message: "New" custom message for the alarm.

*/
void change_alarm(int alarm_id, char *type, int seconds, char *message) {
    int status; // Holds the return status for error-checking during mutex lock/unlock and condition signaling.
    alarm_t *current; // Pointer to traverse and locate the alarm with the unique alarm_id within alarm_list.

    // Lock alarm_mutex to prevent other threads from modifying alarm_list while changes are being made.
    status = pthread_mutex_lock(&alarm_mutex);
    // If the lock fails, err_abort is called to handle the error.
    if(status != 0) 
        err_abort(status,"Lock mutex"); // Error handling

    
    current = alarm_list; // Initialized to point to the first alarm_t structure in the alarm_list

    // Traverse alarm_list to find the alarm with the specified alarm_id.
    while (current != NULL && current->alarm_id != alarm_id) {
        current = current->link;
    }

    /* The while loop moves through each alarm in the list until it either finds the matching alarm_id 
     or reaches the end of the list. If an alarm with the specified alarm_id was found (current != NULL), it 
     modifies the alarm’s properties with the new values provided by the user. */
    if (current != NULL) {
        strcpy(current->type, type);
        current->seconds = seconds;
        strncpy(current->message, message, (MAX_MESSAGE_LENGTH + 1));
        current->is_type_changed = 1; // Flag this alarm as changed
        
        // Prints the change details of the modified alarm.
        printf("Alarm(%d) Changed at %ld: %s %d %s\n", alarm_id, current_time(), type, seconds, message);

    } else { // If no alarm with the specified alarm_id was found (current == NULL), an error message is printed.
        fprintf(stderr, "Error: Alarm ID %d not found\n", alarm_id);
    }
    // Releases alarm_mutex, allowing other threads to access alarm_list.
    status = pthread_mutex_unlock(&alarm_mutex);
    // If unlocking fails, err_abort handles the error.
    if(status != 0)
        err_abort(status,"Unlock mutex");
}

/* The cancel_alarm function locates a specified alarm by alarm_id in the alarm_list linked list. 
If found, it sets a cancellation flag for the alarm and then waits until any associated display thread acknowledges the cancellation. 
Finally, it removes the canceled alarm from the list.
Parameters:
	-   alarm_id: Unique identifier for the alarm.
*/
void cancel_alarm(int alarm_id) {
    int status; // Holds the return status for error-checking during mutex lock/unlock and condition signaling.
    alarm_t **prev, *current; // Pointers for traversing the linked list.
    // Lock the alarm_mutex to ensure exclusive access to the alarm_list while modifying it.
    status = pthread_mutex_lock(&alarm_mutex);
    // pthread_mutex_lock returns a non-zero value on failure; if this happens, err_abort handles the error.
    if (status != 0)
        err_abort(status, "Lock mutex"); // Error handling.

    // Traverse the list to find the alarm with the specified alarm_id
    prev = &alarm_list; // prev starts at the head of the list (&alarm_list). 
    current = alarm_list; // current starts at the first element.

    /*The loop continues as long as current is not NULL and current->alarm_id (the alarm_id of the alarm to which
    the pointer, current, is) pointing  doesn’t match the alarm_id we want to cancel. By the end of the loop,
    current will either point to the target alarm (if found) or NULL (if not found).*/
    while (current != NULL && current->alarm_id != alarm_id) {
        prev = &current->link; // prev always points to the link field of the previous alarm (or alarm_list), allowing for easy removal if needed.
        current = current->link;
    }
     // If the alarm with alarm_id was found, the cancellation process continues.
    if (current != NULL) {
        // Set cancellation flag so display threads can detect it, indicating that this alarm has been marked for cancellation.
        current->is_canceled = 1; 
        /*A cancellation message is printed to log the event, including details such as 
        alarm ID, cancellation time, type, duration, and message. */
        printf("Alarm(%d) Cancelled at %ld: %s %d %s\n", alarm_id, current_time(), current->type, current->seconds, current->message);
        
        /* Signal display threads in case they are waiting on this alarm to wake up any display threads 
        that may be waiting on the display_cond condition variable. This allows those threads to process the cancellation.
        */
        status = pthread_cond_signal(&display_cond); 
        // Error handling is provided in case signaling fails.
        if (status != 0)
            err_abort(status, "Signal cond"); // Error handling

        /* Wait for display thread to acknowledge cancellation. The while loop continues 
        until current->display_thread_id is 0, indicating that the display thread has completed its processing.
        */
        while (current->display_thread_id != 0) {
            /* pthread_cond_wait is called on display_cond and alarm_mutex, 
            effectively pausing until the display thread signals back after finishing its cleanup for the alarm.
            */
            status = pthread_cond_wait(&display_cond, &alarm_mutex);
            // If conditional wait fails, err_abort handles the error printing the assigned message and terminates the program.
            if (status != 0) 
                err_abort(status, "Wait on cond"); // Error handling. 
        }

        // Remove the alarm from the list after display thread has acknowledged it
        *prev = current->link; // Bypasses the current node, removing it from the list.
        free(current); // Deallocates the memory associated with the alarm.

    } else { // If the alarm with the specified alarm_id is not found, an error message is printed, indicating that no alarm exists with that ID.
        fprintf(stderr, "Error: Alarm ID %d not found\n", alarm_id);
    }
    // Releases alarm_mutex, allowing other threads to access alarm_list.
    status = pthread_mutex_unlock(&alarm_mutex);
    // If unlocking fails, err_abort handles the error.
    if (status != 0)
        err_abort(status, "Unlock mutex");
}

// View all alarms in the alarm list, grouped by display thread.
/* The view_alarms function displays all alarms currently in the alarm_list, grouping them by their assigned display thread. 
This is achieved by first identifying unique display_thread_ids and then printing the alarms associated with each. The function
uses no parameters. */
void view_alarms() {
    alarm_t *current; // A pointer used to traverse the alarm_list linked list.
    int status; // Holds the return status for error-checking when locking/unlocking the mutex.
    int display_thread_counter; // Used to keep track of the number of display threads printed, starting from 1.
    int printed_count; // Keeps track of how many unique display threads have already been printed.
    pthread_t printed_threads[100];  // An array to store display_thread_ids that have already been printed, preventing duplicates.

    // Lock alarm_mutex to ensure exclusive access to the alarm_list, preventing other threads from modifying the list.
    status = pthread_mutex_lock(&alarm_mutex);
    // Check if the pthread_mutex_lock call succeeded; if not, err_abort handles the error.
    if (status != 0)
        err_abort(status, "Lock mutex"); // Error handling.

    // Prints the current time in seconds since the Unix Epoch to indicate when the view_alarms command was invoked.
    printf("View Alarms at %ld:\n", current_time());

    
    current = alarm_list; // Initialize the pointer  current to point to the start of the alarm_list.
    display_thread_counter = 1; // Set display_thread_counter to 1, as this will be used to number each unique display thread.
    printed_count = 0; /* Initialize printed_count to 0 to keep track of the number of unique display threads printed so far.
    We need printed_count to avoid duplicates*/

    // Traverse through the alarm list, grouping by display_thread_id
    while (current != NULL) {
        /* Only consider alarms with assigned display threads since a zero value indicates
         that no thread is currently responsible for displaying this alarm. */
        if (current->display_thread_id != 0) {
            bool already_printed = false; // A flag to check if the current display_thread_id has already been printed.
            
            /* Check if this display thread ID has already been printed. A for loop iterates over printed_threads,
            comparing each stored display_thread_id with display_thread_id of the current alarm in the list.
            */
            for (int i = 0; i < printed_count; i++) {
                /* If a match is found, already_printed is set to true, and the loop breaks, 
                avoiding duplicate output for the same display_thread_id.
                */
                if (printed_threads[i] == current->display_thread_id) {
                    already_printed = true;
                    break;
                }
            }
            // If already_printed is false, it means this display_thread_id hasn’t been printed yet.
            if (!already_printed) {
                // Print display thread info if it hasn't been printed before
                printf("%d. Display Thread %ld Assigned:\n", display_thread_counter, current->display_thread_id);
                // printed_threads[printed_count] = current->display_thread_id stores the printed display_thread_id in the printed_threads array.
                printed_threads[printed_count] = current->display_thread_id; // Mark as printed
                printed_count++; // printed_count is incremented to track the number of unique display threads printed.

                // Print alarms assigned to this display thread
                int alarm_counter = 1; // initialized to 1 to label each alarm under the current display thread.

                // temp is a pointer used to traverse the alarm_list again to find all alarms assigned to the current display_thread_id.
                alarm_t *temp = alarm_list; 

                /*If temp->display_thread_id matches the current display_thread_id, it prints details about the alarm 
                (ID, type, seconds, and message) in the format X.a, X.b, where X is the display thread number, 
                and a, b, ... are sub-labels for each alarm.
                */
                while (temp != NULL) {
                    if (temp->display_thread_id == current->display_thread_id) {
                        printf("%d%c. Alarm(%d): %s %ld %s\n", display_thread_counter, 'a' + (alarm_counter - 1),
                               temp->alarm_id, temp->type, temp->seconds, temp->message);
                        alarm_counter++; // The alarm_counter increments for each alarm under the same display thread.
                    }
                    temp = temp->link; // Move the pointer temp to the next element in the alarm_list linked list
                }
                // increments after processing each unique display thread, ensuring each thread group has a unique numbering.
                display_thread_counter++; 
            }
        }
        current = current->link; // After processing alarms for one display_thread_id, move current to the next alarm in the list.
    }

    // Unlock alarm_mutex after reading the list to allow other threads access.
    status = pthread_mutex_unlock(&alarm_mutex);
    // 	If unlocking fails, err_abort handles the error.
    if (status != 0)
        err_abort(status, "Unlock mutex");
}

/* The display_thread function manages a thread responsible for displaying messages from alarms that match a specified alarm type.
 It periodically checks for alarms that are assigned to it and prints messages for active alarms until they either 
 expire, are canceled, or change type. If no alarms remain active for this thread, it exits.
*/
void *display_thread(void *arg) {
    alarm_t *alarm = (alarm_t *)arg; // A pointer to an alarm_t structure that the thread will initially manage.
    alarm_t *current; // A pointer used to traverse the alarm_list linked list.
    int status; // Hold the return status for error-checking when locking/unlocking the mutex and handling condition variables.
    char alarm_type[MAX_TYPE_LENGTH + 1]; // A string to store the type of alarm this thread is responsible for managing.

    // Initializes alarm_type with the type of the passed-in alarm so that the thread only monitors alarms of this type.
    strcpy(alarm_type, alarm->type); 
    
    /* Locks alarm_mutex to ensure exclusive access to alarm_list, preventing other threads from modifying 
    the list while this thread is checking and updating it.
     */
     status = pthread_mutex_lock(&alarm_mutex);
    // If locking fails, err_abort handles the error and terminates the program.
        if(status != 0)
            err_abort(status, "Lock mutex"); // Error handling

    // An infinite while loop that continues until there are no active alarms assigned to this display thread.
    while (1) {
        // A flag is set to 0 at the start of each loop iteration. It will be set to 1 if an active alarm for this thread is found.
        int active_alarm_flag = 0;
        current = alarm_list; // Initialized to point to the head of alarm_list for traversal.

        // Check alarms in the list assigned to this display thread
        // The inner while loop iterates over each alarm in alarm_list.
        while (current != NULL) {
            /* Checks if the current alarm matches alarm_type and is assigned to this display thread 
            (display_thread_id == pthread_self()). If both conditions are met, this alarm is considered “active” for this thread,
             so active_alarm_flag is set to 1 to indicate that the thread should continue running.
            */
            if (strcmp(current->type, alarm_type) == 0 && current->display_thread_id == pthread_self()) {
                active_alarm_flag = 1; // active_alarm_flag is set to 1 to indicate that the thread should continue running.

                // Expire the alarm if its time has elapsed
                //Checks if the current alarm has expired by comparing the sum of insert_time and seconds with current_time().
                if (current->insert_time + current->seconds <= current_time()) {
                    // If expired, it prints a message indicating that this alarm has expired and stops printing
                    printf("Alarm(%d) Expired; Display Thread (%ld) Stopped Printing Alarm Message at %ld: %s %d %s\n",
                           current->alarm_id, pthread_self(), current_time(), current->type, current->seconds, current->message);
                    current->display_thread_id = 0; // Sets display_thread_id to 0 to indicate that this alarm is no longer managed by any display thread. 
                    current->is_expired = 1; // Sets is_expired to 1 so that the main thread can remove this alarm from alarm_list.
                    current = current->link; // Update the pointer traversing the alarm_list linked list
                    status = pthread_cond_signal(&alarm_cond); // Signals alarm_cond to notify the main thread that an alarm has expired.
                    if (status != 0)
                        err_abort(status, "Signal cond");
                    break; //  Breaks out of the inner while loop to check for other alarms after updating the expired alarm.
                }

                // Handle alarm cancellation
                // Checks if the current alarm has been canceled (is_canceled == 1).
                if (current->is_canceled) {
                    // If canceled, prints a message indicating that the alarm is canceled and stops displaying its message.
                    printf("Alarm(%d) Cancelled; Display Thread (%ld) Stopped Printing Alarm Message at %ld: %s %d %s\n",
                           current->alarm_id, pthread_self(), current_time(), current->type, current->seconds, current->message);

                    current->display_thread_id = 0;  // Set to 0 to signal cancellation processing is complete

                    // Signal cancel_alarm to allow it to proceed with removal
                    status = pthread_cond_signal(&display_cond);
                    if(status != 0)
                        err_abort(status, "Signal cond");
                    current = current->link; // Moves current to the next alarm in the list 
                    break; // Breaks out of the loop to check other alarms after handling this cancellation.
                }
                // Type Change Check
                // Checks if the type of the current alarm has changed (is_type_changed == 1).
                if (current->is_type_changed) {
                    // If the type has changed, it prints a message indicating the type change.
                    printf("Alarm(%d) Changed Type; Display Thread (%ld) Stopped Printing Alarm Message at %ld: %s %d %s\\n",
                           current->alarm_id, pthread_self(), current_time(), current->type, current->seconds, current->message);
                    current->display_thread_id = 0; // Sets display_thread_id to 0, signaling that this alarm is no longer assigned to this thread.
                    current->is_type_changed = 0; // Sets is_type_changed back to 0 since the change has been handled.
                    current = current->link; // Moves current to the next alarm in the list
                    continue; // Uses continue to skip the rest of the loop for this alarm, moving directly to the next alarm.
                }


                // Periodically print the alarm message
                /* If none of the above conditions are met (the alarm is active and neither expired, 
                canceled, nor type-changed), it prints the alarm’s message periodically.
                */
                printf("Alarm(%d) Message PERIODICALLY PRINTED BY Display Thread (%ld) at %ld: %s %d %s\n",
                       current->alarm_id, pthread_self(), current_time(), current->type, current->seconds, current->message);
            }
            /* After processing the current alarm (or skipping it if it doesn’t match the type and thread ID), 
            current is set to the next alarm in the list.
            */
            current = current->link;
        }

       // Wait 5 seconds before the next check
       struct timespec cond_time;
       cond_time.tv_sec = time(NULL);
       cond_time.tv_sec += 5;

        // waits on display_cond for a signal or a 5-second timeout.
        status = pthread_cond_timedwait(&display_cond, &alarm_mutex, &cond_time); 
        // If it times out (ETIMEDOUT), the loop resumes. If there’s an error, err_abort handles it.
        if(status != 0 && status != ETIMEDOUT)
            err_abort (status, "Cond timedwait"); // Error handling.

        // If no active alarms remain, terminate the thread
        // If active_alarm_flag remains 0 after checking all alarms, this means there are no active alarms left for this thread.
        if (!active_alarm_flag) { 
            // The thread prints a termination message 
            printf("Display Thread Terminated (%ld) at %ld: %s\n", pthread_self(), current_time(), alarm_type);
            // Unlocks alarm_mutex
            status = pthread_mutex_unlock(&alarm_mutex);
            if(status != 0)
                err_abort(status, "Unlock mutex");
            pthread_exit(NULL); // Terminates the current display thread if there is no active alarms left
            
        }
    }
    // Ensures alarm_mutex is unlocked if the function somehow exits the loop.
    status = pthread_mutex_unlock(&alarm_mutex);
    if(status != 0)
                err_abort(status, "Unlock mutex");
}


/* alarm_thread routine function manages the reassignment of alarms to display threads. When an alarm type changes
 or a new alarm is added, this thread ensures that it’s assigned to an appropriate display thread, 
 creating new threads if necessary.
*/
void *alarm_thread(void *arg) {

    /*runs an infinite loop, continuously monitoring alarm_list for alarms needing reassignment.
     It only acts when signaled, remaining dormant otherwise.
    */
    while (1) {
        int status; // Stores return codes from locking/unlocking mutexes and condition waits.

        // The pointer current is used to traverse alarm_list to identify alarms needing reassignment.
        // The pointer temp is used to find existing display threads for a specific alarm type.
        alarm_t *current, *temp;
        pthread_t match_display_thread_id; // Stores the thread ID of a suitable display thread for an alarm.

        // Locks alarm_mutex to ensure exclusive access to alarm_list during modifications.
        status = pthread_mutex_lock(&alarm_mutex);
        // If locking fails, err_abort handles the errors.
        if (status != 0)
            err_abort(status, "Lock mutex"); // Error handling.

        // Wait for signal to check alarms
        // waits on alarm_cond until a signal is received. This could happen if an alarm type changes, an alarm is added, or an alarm needs reassignment.
        status = pthread_cond_wait(&alarm_cond, &alarm_mutex);
        // If waiting for a conditional variable fails, err_abort handles the error.
        if (status != 0)
            err_abort(status, "Wait on cond"); // Error handling.

        current = alarm_list; // Initialized to point to the head of alarm_list.

        // Iterates through alarms for reassignment
        while (current != NULL) {

             // Skip already expired or canceled alarms early
            if (current->is_expired || current->is_canceled) {
                current = current->link;
                continue;
            }
            /* Checks if an alarm needs reassignment. Reassignment is required if:
                    1- is_type_changed is set to 1 (indicating a type change).
                    2- display_thread_id is 0 (indicating that the alarm has not yet been assigned).
            */
            if (current->is_type_changed || current->display_thread_id == 0) {
                current->display_thread_id = 0;  // Reset display thread to remove current assignment
                current->is_type_changed = 0;     // Clear the type-changed flag

                // For the search of an existing display thread we need the following flags and place holders
                int found_thread = 0; //Initialized to 0 indicating no available thread yet.
                int full_thread_found = 0; // Initializes found_thread to 0 (indicating whether all threads are fully occupied).
                match_display_thread_id = 0; // set to 0, as no thread has been matched.
                temp = alarm_list; // Initialized to traverse alarm_list to search for a suitable display thread.

                // Step 1: Search for an existing display thread for the new type with fewer than 2 alarms
                // This while loop iterates over each alarm in alarm_list (with temp pointing to each alarm in turn).
                while (temp != NULL) {
                    /* The if statement checks two conditions:
	                    - strcmp(temp->type, current->type) == 0: Verifies that temp has the same type as current. This ensures that only alarms of the same type are grouped together under a single display thread.
	                    - temp->display_thread_id != 0: Confirms that temp is already assigned to a display thread.
                       If both conditions are met, the code proceeds to count the alarms assigned to this thread.
                    */
                    if (strcmp(temp->type, current->type) == 0 && temp->display_thread_id != 0) {
                        // Initializes thread_alarm_count to 0 to trach the number of alarms of the same type in each uniqe display thread
                        int thread_alarm_count = 0; 
                        // Iterates through alarm_list with inner_temp pointer to count the number of alarms assigned to temp->display_thread_id.
                        alarm_t *inner_temp = alarm_list;

                        // Count alarms assigned to this display thread
                        // The inner while loop iterates over alarm_list.
                        while (inner_temp != NULL) {
                            // Each time the loop finds an alarm with the same display_thread_id as temp, it increments thread_alarm_count.
                            if (inner_temp->display_thread_id == temp->display_thread_id) {
                                thread_alarm_count++;
                            }
                            inner_temp = inner_temp->link; // Inner_temp moves to the next alarm in alarm_list.
                        }

                        // If a thread has fewer than 2 alarms, use it
                        /* After counting the alarms assigned to temp->display_thread_id, 
                        it checks if this thread has reached its maximum allowed alarms (MAX_ALARMS_PER_THREAD),
                        which is a maximum of two alarms of the same type of each display thread in our case.
                        */
                        if (thread_alarm_count < MAX_ALARMS_PER_THREAD) {
                            // Sets match_display_thread_id to temp->display_thread_id, marking it as an available thread for current.
                            match_display_thread_id = temp->display_thread_id; 
                            found_thread = 1; // Sets found_thread to 1 to indicate that a suitable thread has been found.
                            break; // break exits the loop because a match has been identified.

                        } else { // If thread_alarm_count equals or exceeds MAX_ALARMS_PER_THREAD:

                            /* Marking that a fully occupied thread was found for this type, 
                            which may require creating a new thread if no partially filled thread is available.
                            */
                            full_thread_found = 1; // Mark that a full thread for this type was found.
                        }
                    }
                    temp = temp->link; // The pointer temp moves to the next alarm in alarm_list.
                }

                // Step 2: Assign the alarm to an existing or new display thread as needed
                
                /* If an available thread was found (found_thread is 1):
	                - Assigns current->display_thread_id to match_display_thread_id.
	                - Prints a message indicating that the alarm was assigned to this display thread, along with its details.
                */
                if (found_thread) {
                    // Assign current alarm to the found thread with available space
                    current->display_thread_id = match_display_thread_id;
                    printf("Alarm (%d) Assigned to Display Thread (%ld) at %ld: %s %d %s\n",
                           current->alarm_id, match_display_thread_id, current_time(), current->type,
                           current->seconds, current->message);
                
                /* If no available thread was found but a fully occupied thread for the alarm type exists:
	                - Creates an additional display thread using pthread_create, assigning it to current->display_thread_id.
                    - Prints a message indicating that a new display thread was created to manage current.
                */
                } else if (full_thread_found) {
                    // Step 3: If all threads of this type are full, create an additional display thread
                    status = pthread_create(&current->display_thread_id, NULL, display_thread, current);
                    if (status != 0)
                        err_abort(status, "Create display thread");

                    printf("Additional New Display Thread (%ld) Created at %ld: %s %d %s\n",
                           current->display_thread_id, current_time(), current->type,
                           current->seconds, current->message);

                /* If no thread for this type exists (i.e., neither found_thread nor full_thread_found is true):
                    - Creates the first display thread for this alarm type, assigning it to current->display_thread_id.
                    - Prints a message indicating the creation of the first display thread for this type.
                */
                } else {
                    // No thread for this type exists, create the first one
                    status = pthread_create(&current->display_thread_id, NULL, display_thread, current);
                    if (status != 0)
                        err_abort(status, "Create display thread");

                    printf("First New Display Thread (%ld) Created at %ld: %s %d %s\n",
                           current->display_thread_id, current_time(), current->type,
                           current->seconds, current->message);
                }
            }
            current = current->link; // Advances current to the next alarm in alarm_list to continue the reassignment process.
        }
        // After processing all alarms in alarm_list, alarm_mutex is unlocked, allowing other threads to access alarm_list.
        status = pthread_mutex_unlock(&alarm_mutex);
        // If unlocking fails, err_abort handles the error.
        if(status != 0)
            err_abort(status, "Unlock mutex");
    }
    return NULL; //
}


// Main function to handle user input, create the alarm thread, and check for expired alarms

/* The main function is responsible for handling user inputs and performing periodic alarm expiration handling. 
It continuously prompts for user input, parse and executes commands, and checks for expired alarms,
maintaining a responsive loop with a small sleep delay to manage CPU usage effectively.
*/
int main() {
    int status; // Stores return codes from locking/unlocking mutexes, signaling and thread creation.
    int alarm_id; // Hold the unique alarm_id for the parsed alarm
    int seconds; // Hold the duration of alarm is seconds.
    char line[1024]; // Buffer to hold the user’s input.
    char type[MAX_TYPE_LENGTH + 1]; //Holds the alarm type, with a maximum length defined by MAX_TYPE_LENGTH + 1 (including null terminator).
    char message[MAX_MESSAGE_LENGTH + 1]; // Holds the message for the alarm.
    pthread_t alarm_thread_id; // Holds the thread ID for the alarm handling thread.

    alarm_t **prev, *current; // Pointers used to traverse and modify the linked list of alarms.
    fd_set read_fds; // Set of file descriptors for monitoring user input.
    struct timeval timeout; // Specifies the select() timeout interval for checking alarm expiration.

    // Creates a separate thread dedicated to alarm handling
    status = pthread_create(&alarm_thread_id, NULL, alarm_thread, NULL);
    // If pthread_create fails, the function err_abort is called with an error message.
    if (status != 0) 
        err_abort(status, "Create alarm thread"); // Error hhandling

    // Main loop continuously listens for user input or a timeout event.
    while (1) {
        FD_ZERO(&read_fds); // clears read_fds, ensuring it’s reset on each loop iteration.
        FD_SET(STDIN_FILENO, &read_fds); // adds standard input to read_fds to monitor user input.

        // Set the timeout interval for periodic alarm expiration checks
        timeout.tv_sec = 5;
        timeout.tv_usec = 0;

        printf("Alarm> ");
        // Wait for user input or timeout for expiration check
        int result = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &timeout);

        if (result == -1) { // An error occurred during select().
            fprintf(stderr,"select failed");
            continue; //skip to the next iteration
        } else if (result == 0) { // Timeout triggered (no input), so the function proceeds to the alarm expiration check.

            // Locks alarm_mutex to ensure exclusive access to alarm_list during modification.
            status = pthread_mutex_lock(&alarm_mutex);
            // If locking fails, err_abort handles the error
            if (status != 0) 
                err_abort(status, "Lock mutex");

            prev = &alarm_list; //initialzed to the the header of the linked list.
            current = alarm_list; //initialized to the item (alarm) stored in the header of the linked list.

            while (current != NULL) {
                /* For each alarm: If not canceled and expired (current->insert_time + current->seconds <= current_time()), 
                it’s removed.
                */
                if (!current->is_canceled && (current->insert_time + current->seconds <= current_time())) {
                    printf("Alarm(%d): Removing expired alarm from list at %ld\n",
                           current->alarm_id, current_time());
                    /* The expired alarm is removed from the list by updating the previous alarm’s link to bypass it and point to
                    the alarm after the expired alarm in the linked list
                    */
                    *prev = current->link;
                    free(current); // Freeing the memory of the expired alarm.
                    current = *prev; //Current point to the previous item in the linked list.
                } else { /* Continue searching the alarm list for expired alarms if the current alarm doesn't meet 
                        the is_expired and not canceled conditions. 
                        */

                    prev = &current->link; // Points to the address of of the address of the next alarm in the linked list
                    current = current->link; // Points to the address of the next item in the linked list
                }
            }

            // Signal alarm thread after checking expired alarms to notify of any changes
            status = pthread_cond_signal(&alarm_cond); // Ensure alarm_thread is notified
            if(status != 0)
                err_abort(status, "Signal cond");

            // Releases the mutex after finishing with alarm_list, allowing other threads to access it.
            status = pthread_mutex_unlock(&alarm_mutex);
            if (status != 0) 
                err_abort(status, "Unlock mutex");

        // If input is detected on STDIN_FILENO, it reads the user command using fgets.       
        } else if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            // User input is available
            if (fgets(line, sizeof(line), stdin) == NULL) exit(0);
            // If the input line is empty, it skips further processing.
            if (strlen(line) <= 1) continue;
            // // If the command begins with "Start_Alarm", it proceeds to parse the format.
            if (strncmp(line, "Start_Alarm", 11) == 0) {
                // Uses sscanf to extract alarm_id, type, seconds, and message from the input.
                if (sscanf(line, "Start_Alarm(%d): T%3s %d %128[^\n]", &alarm_id, type + 1, &seconds, message) < 4) {
                    // If parsing fails (< 4), it prints “Bad Command” and skips further processing.
                    fprintf(stderr, "Bad Command\n");
                } else { // If sscanf completes the extraction process successfuly

                    // Sets type[0] to 'T' to ensure the type always begins with “T”, and null terminates type.
                    type[0] = 'T';
                    type[4] = '\0';
                    //calls insert_alarm to add it to the alarm list linked list.
                    insert_alarm(alarm_id, type, seconds, message);

                    status = pthread_cond_signal(&alarm_cond); // Notify alarm_thread of new alarm
                    if(status != 0)
                        err_abort(status, "Signal cond");
                }
            // checks if the command begins with "Change_Alarm".
            } else if (strncmp(line, "Change_Alarm", 12) == 0) {
                // // sscanf parses the input, with the same error handling as in the Start_Alarm case if it fails
                if (sscanf(line, "Change_Alarm(%d): T%3s %d %128[^\n]", &alarm_id, type + 1, &seconds, message) < 4) {
                    fprintf(stderr, "Bad Command\n");
                } else { // Calls change_alarm to modify an existing alarm.
                    type[0] = 'T';
                    type[4] = '\0';
                    change_alarm(alarm_id, type, seconds, message);
                    status = pthread_cond_signal(&alarm_cond); // Notify alarm_thread of changed alarm
                    if(status != 0)
                        err_abort(status, "Signal cond");

                }
            // Checks for the "Cancel_Alarm" command and parses only alarm_id.
            } else if (strncmp(line, "Cancel_Alarm", 12) == 0) {
                if (sscanf(line, "Cancel_Alarm(%d)", &alarm_id) < 1) {
                    fprintf(stderr, "Bad Command\n");
                } else { // calls cancel_alarm to cancel an alarm in the alarm list
                    cancel_alarm(alarm_id);
                    status = pthread_cond_signal(&alarm_cond); // Notify alarm_thread of cancellation
                    if(status != 0)
                        err_abort(status, "Signal cond");
                }
            // If the command is "View_Alarms", it directly calls view_alarms to display the current alarms grouped by display thread.
            } else if (strncmp(line, "View_Alarms", 11) == 0) {
                view_alarms();

            } else { // // For any other input that doesn’t match the specified request formats, it prints an “Invalid request” error message
                printf("Error: Invalid request\n");
            }
        }
    }
    return 0;
}