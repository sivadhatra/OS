#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <assert.h>

#include <pthread.h>
#include <semaphore.h>

#define CHECK( val ) do { \
    if (val != 0) \
        assert(0); \
} while(0) \

struct Student
{
    sem_t tutorAvailableSemaphore; // used to allow a tutor to wake up a specific student
    int numHelpsRemaining;
    int tutor;                     // tutor id student was helped by
};

struct Tutor
{
    int numSessions; // used to keep track of how many tutoring sessions 
};

struct Chair 
{
    int studentId;    // used to know which student is occupying the current chair  
    int visitorToken; // nth request for help (used to keep track of priority)
};

struct PriorityQueue
{
    int studentId;       // used to let a tutor know which student in gStudentArray to tutor
    int helpsRemaining;  // used to help a tutor pick the student with the highest priority
    int visitorToken;    // used to help a tutor pick the student who arraived first (used when priorities are the same)
};

pthread_mutex_t gNumChairsMutex;
int gNumChairs;
int gNumChairsOccupied = 0;
pthread_mutex_t gChairArrayMutex;
struct Chair* gChairArray;

int gNumStudentsAtStart;
struct Student* gStudentArray;

int gNumTutorsAtStart;
struct Tutor* gTutorArray;

pthread_mutex_t gPriorityTableMutex;
struct PriorityQueue* gPriorityTable;

pthread_mutex_t tutorMutex;         
sem_t gStudentArrivedSemaphore;   // student -> coordinator sync
sem_t coordinatorNotifySemaphore; // coordinator -> student sync

//Array used for storing whether that new student got added to the priority queue or not
int* markings;

int gStudentsLeft = 1;
int gNumHelpsRequiredAtStart;

int totalCounts = 0; // total # of visits by students
int totalTutoredCount;
int currentWorkingTutors;

void* studentThreadFunc(void* id)
{
    int studentId = *((int *) id);
    struct Student* student = &gStudentArray[studentId];

    while (student->numHelpsRemaining > 0)
    {
        // Simulate "programming" for up to two ms
        usleep(rand() % 2000);

        // Add student to end of queue (shared data structure between student and coordinator)
        CHECK(pthread_mutex_lock(&gChairArrayMutex));
        {

            // No chairs available
            if(gNumChairsOccupied >= gNumChairs)
            {
                CHECK(pthread_mutex_unlock(&gChairArrayMutex));
                printf("S: Student %d found no empty chair. Will try again later.\n", studentId);
                continue;
            }

            gNumChairsOccupied++;
            totalCounts++;

            for(int i = 0; i < gNumChairs; i++)
            {
                if(gChairArray[i].studentId == -1)
                {
                    gChairArray[i].studentId = studentId;
                    gChairArray[i].visitorToken = totalCounts;
                    printf("S: Student %d takes a seat. Empty chairs = %d.\n", studentId, gNumChairs-gNumChairsOccupied);
                    break;
                }
            }

            // Tell coordinator "Hey, I'm here!". Ordering of the post/unlock matter.
            CHECK(sem_post(&gStudentArrivedSemaphore));
        }

        CHECK(pthread_mutex_unlock(&gChairArrayMutex));
        
        // Wait for available tutor
        CHECK(sem_wait(&student->tutorAvailableSemaphore));

        // Simulate "tutoring" for two ms
        usleep(200);

        printf("S: Student %d received help from Tutor %d.\n", studentId, student->tutor);
    }

    pthread_exit(NULL);
}

void* coordinatorThreadFunc()
{
    int totalreq = 0;

    while (gStudentsLeft)
    {
        // Wait until student says "Hey, I need a tutor!"
        CHECK(sem_wait(&gStudentArrivedSemaphore));

        CHECK(pthread_mutex_lock(&gChairArrayMutex));

        for(int i = 0; i < gNumChairs; i++)
        {
            // If chair has a student and if the student has not already been seen
            if(gChairArray[i].studentId > -1 && markings[i] == -1)
            {
                totalreq++;
                markings[i] = gChairArray[i].studentId;

                printf("C: Student %d with priority %d added to the queue. Waiting students now = %d. Total " \
               "requests = %d\n", gChairArray[i].studentId, gStudentArray[gChairArray[i].studentId].numHelpsRemaining, gNumChairsOccupied,totalreq);

                CHECK(pthread_mutex_unlock(&gChairArrayMutex));
                CHECK(pthread_mutex_lock(&gPriorityTableMutex));

                gPriorityTable[i].studentId      = gChairArray[i].studentId;
                gPriorityTable[i].helpsRemaining = gStudentArray[gChairArray[i].studentId].numHelpsRemaining;
                gPriorityTable[i].visitorToken   = gChairArray[i].visitorToken;

                // Notify tutor that a student is available for tutoring
                CHECK(sem_post(&coordinatorNotifySemaphore));
                CHECK(pthread_mutex_unlock(&gPriorityTableMutex));
                CHECK(pthread_mutex_lock(&gChairArrayMutex));
            }
        }

        CHECK(pthread_mutex_unlock(&gChairArrayMutex));
    }

    pthread_exit(NULL);
}

void* tutorThreadFunc(void* id)
{
    while (gStudentsLeft)
    {
        int tutorId  = *((int *) id);
        struct Tutor* tutor = &gTutorArray[tutorId];

        // Wait for tutor coordinator to say "I need you to tutor student ..."
        CHECK(sem_wait(&coordinatorNotifySemaphore));

        CHECK(pthread_mutex_lock(&tutorMutex));
        currentWorkingTutors += 1;
        CHECK(pthread_mutex_unlock(&tutorMutex));

        int pickedprior = -1;
        int curStudentId = -1;
        int pickedno = 2147483647;
        int ind = -1;

        // Find student to tutor
        CHECK(pthread_mutex_lock(&gPriorityTableMutex));
        {
            // This is super slow, but it is easy and correct. 
            // TODO: We should be using an array of linked-lists where each array[0..numHelps-1] corresponds to a priority level. 
            // Iterate over queued students and choose one with the highest priority. We should be
            for(int i = 0; i < gNumChairs; i++)
            {
                if(gPriorityTable[i].studentId > -1 && gPriorityTable[i].helpsRemaining > pickedprior)
                {
                    curStudentId = gPriorityTable[i].studentId;
                    pickedprior  = gPriorityTable[i].helpsRemaining;
                    pickedno     = gPriorityTable[i].visitorToken;
                    ind          = i;
                }
                else if(gPriorityTable[i].studentId > -1 && gPriorityTable[i].helpsRemaining == pickedprior && gPriorityTable[i].visitorToken < pickedno)
                {
                    curStudentId = gPriorityTable[i].studentId;
                    pickedno     = gPriorityTable[i].visitorToken;
                    ind          = i;
                }
            }

            gPriorityTable[ind].studentId = -1;
        }
        CHECK(pthread_mutex_unlock(&gPriorityTableMutex));

        CHECK(pthread_mutex_lock(&gChairArrayMutex));

            struct Student* curStudent = &gStudentArray[curStudentId];
            gChairArray[ind].studentId = -1;
            markings[ind] = -1;
            gNumChairsOccupied--;
            curStudent->numHelpsRemaining = curStudent->numHelpsRemaining - 1;
            curStudent->tutor = tutorId;

            // Tell student "I'm available to start tutoring"
            CHECK(sem_post(&curStudent->tutorAvailableSemaphore));

        CHECK(pthread_mutex_unlock(&gChairArrayMutex));

        // Simulate "tutoring"
        usleep(200);

        tutor->numSessions++;

        CHECK(pthread_mutex_lock(&tutorMutex));
        totalTutoredCount++;
        currentWorkingTutors--;
        //! Assuming current tutor thread is finished tutoring.
        printf("T: Student %d tutored by Tutor %d. Students tutored now = %d. Total sessions tutored = %d\n", curStudentId, tutorId, currentWorkingTutors, totalTutoredCount);
        CHECK(pthread_mutex_unlock(&tutorMutex));
    }
    pthread_exit(NULL);
}

int main(int argc, char* argv[])
{
    assert(argc == 5);
    assert(argv[1] > 0 && argv[2] > 0 && argv[3] > 0 && argv[4] > 0);

    gNumStudentsAtStart = (int)atoi(argv[1]);
    gStudentArray = (struct Student*)malloc(sizeof(struct Student) * gNumStudentsAtStart);

    gNumTutorsAtStart = (int)atoi(argv[2]);
    gTutorArray = (struct Tutor*)malloc(sizeof(struct Tutor) * gNumTutorsAtStart);

    gNumChairs = (int)atoi(argv[3]);
    CHECK(pthread_mutex_init(&gNumChairsMutex, NULL));

    // Initialize "gStudentArrivedSemaphore" to 0 because the coordinator thread should
    // block until a student thread posts/signals
    CHECK(sem_init(&gStudentArrivedSemaphore, 0, 0));
    
    // Initialize "coordinatorNotifySemaphore" to 0 because the coordinator thread should
    // block until a student thread posts/signals
    CHECK(sem_init(&coordinatorNotifySemaphore, 0, 0));

    gNumHelpsRequiredAtStart = (int)atoi(argv[4]);

    // All students start out with specified number of helps
    // Initialize "tutorAvailableSemaphore" to 0 because a student thread should
    // block until a tutor threads posts/signals
    for (int i = 0; i < gNumStudentsAtStart; ++i)
    {
        gStudentArray[i].numHelpsRemaining = gNumHelpsRequiredAtStart;
        CHECK(sem_init(&gStudentArray->tutorAvailableSemaphore, 0, 0));
    }

    // All tutors initially have taught zero sessions
    for (int i = 0; i < gNumTutorsAtStart; ++i)
    {
        gTutorArray[i].numSessions = 0;
    }
    
    markings = (int*)malloc(sizeof(int) * gNumChairs);
    gPriorityTable = (struct PriorityQueue*)malloc(sizeof(struct PriorityQueue) * gNumChairs);
    totalTutoredCount=0;
    currentWorkingTutors=0;

    // All chairs start out unoccupied
    // And there is no one "waiting to be tutored"
    gChairArray = (struct Chair*)malloc(sizeof(struct Chair) * gNumChairs);
    for (int i = 0;i < gNumChairs; i++)
    {
        gChairArray[i].studentId         = -1;
        gChairArray[i].visitorToken      = -1;
        markings[i]                      = -1;
        gPriorityTable[i].studentId      = -1;
        gPriorityTable[i].helpsRemaining = -1;
    }

    CHECK(pthread_mutex_init(&gPriorityTableMutex, NULL));
    CHECK(pthread_mutex_init(&tutorMutex, NULL));
    CHECK(pthread_mutex_init(&gChairArrayMutex, NULL));

    // **************** INIT END **************** //

    // Thread Creation/Spawn
    pthread_t* studentThreads = (pthread_t*)(malloc(sizeof(pthread_t) * gNumStudentsAtStart));
    for (int i = 0; i < gNumStudentsAtStart; ++i)
    {
        int *arg = malloc(sizeof(*arg));
        if ( arg == NULL ) {
            fprintf(stderr, "Couldn't allocate memory for thread arg.\n");
            exit(EXIT_FAILURE);
        }
        *arg = i;
        CHECK(pthread_create(&studentThreads[i], NULL, studentThreadFunc, arg));
    }

    pthread_t coordinatorThread;
    CHECK(pthread_create(&coordinatorThread, NULL, coordinatorThreadFunc, NULL));
    
    pthread_t* tutorThreads = (pthread_t*)(malloc(sizeof(pthread_t) * gNumTutorsAtStart));
    for (int i = 0; i < gNumTutorsAtStart; ++i)
    {
        int *arg = malloc(sizeof(*arg));
        if ( arg == NULL ) {
            fprintf(stderr, "Couldn't allocate memory for thread arg.\n");
            exit(EXIT_FAILURE);
        }
        *arg = i;

        // CHECK(pthread_setname_np(tutorThreads[i], "T"));
        CHECK(pthread_create(&tutorThreads[i], NULL, tutorThreadFunc, arg));
    }

    // **************** THREAD SPAWN END **************** //

    // Wait until last student has finished
    for (int i = 0; i < gNumStudentsAtStart; ++i)
    {
        pthread_join(studentThreads[i], NULL);
    }

    gStudentsLeft = 0;

    // Cleanup
    free(gTutorArray);
    free(gStudentArray);

    return 0;
}
