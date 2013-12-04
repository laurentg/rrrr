/* worker.c : worker processes that handle routing requests */

#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <nanomsg/nn.h>
#include <nanomsg/reqrep.h>
#include <assert.h>
#include "config.h"
#include "rrrr.h"
#include "tdata.h"
#include "hashgrid.h"
#include "router.h"
#include "parse.h"
#include "json.h"

int main(int argc, char **argv) {

    /* SETUP */
    
    // logging
    setlogmask(LOG_UPTO(LOG_DEBUG));
    openlog(PROGRAM_NAME, LOG_CONS | LOG_PID | LOG_PERROR, LOG_USER);
    syslog(LOG_INFO, "worker starting up");
    
    // load transit data from disk
    tdata_t tdata;
    tdata_load(RRRR_INPUT_FILE, &tdata);

    // initialise the hashgrid to map lat/lng to stop indices
    HashGrid hg;
    coord_t coords[tdata.n_stops];
    for (uint32_t c = 0; c < tdata.n_stops; ++c) {
        coord_from_latlon(coords + c, tdata.stop_coords + c);
    }
    HashGrid_init (&hg, 100, 500.0, coords, tdata.n_stops);
    
    // initialize router
    router_t router;
    router_setup(&router, &tdata);
    //tdata_dump(&tdata); // debug timetable file format
    
    // establish nanomsc connection
    int sock = nn_socket(AF_SP, NN_REQ);
    int rc = nn_connect(sock, WORKER_ENDPOINT);
    if (rc < 1) exit(1);
    
    /* MAIN LOOP */
    uint32_t request_count = 0;
    char result_buf[8000];
    while (true) {
        void *msg;
        size_t nbytes = nn_recv (sock, &msg, NN_MSG, 0);
        if (!msg) // interrupted (signal)
            break; 
        if (++request_count % 100 == 0)
            syslog(LOG_INFO, "worker received %d requests\n", request_count);
        char *qstring = (char *) msg;
        printf("%s\n", qstring);
        router_request_t preq;
        parse_request_from_qstring(&preq, &tdata, &hg, qstring);
        router_request_t req;
        req = preq;

        router_route (&router, &req);

        // repeat search in reverse to compact transfers
        uint32_t n_reversals = req.arrive_by ? 1 : 2;
        if (req.start_trip_trip != NONE) n_reversals = 0;
        // n_reversals = 0; // DEBUG turn off reversals

        for (uint32_t i = 0; i < n_reversals; ++i) {
            router_request_reverse (&router, &req); // handle case where route is not reversed
            router_route (&router, &req);
        }
        router_request_dump (&router, &preq);
        router_result_dump(&router, &req, result_buf, 8000);
        printf("%s", result_buf);
 
        struct plan plan;
        router_result_to_plan (&plan, &router, &req);
        plan.req.time = preq.time; // restore the original request time
        uint32_t result_length = render_plan_json (&plan, router.tdata, result_buf, 8000);
        
        nn_send(sock, result_buf, result_length, 0);
    }
    
    /* TEAR DOWN */
    syslog(LOG_INFO, "worker terminating");
    router_teardown(&router);
    tdata_close(&tdata);
    nn_close(sock);
    exit(EXIT_SUCCESS);
}

