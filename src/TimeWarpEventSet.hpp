#ifndef TIME_WARP_EVENT_SET_HPP
#define TIME_WARP_EVENT_SET_HPP

/* This class provides the set of APIs needed to handle events in
   an event set.
 */

#include <vector>
#include <set>
#include <unordered_map>
#include <mutex>
#include <memory>

#include "Event.hpp"
#include "STLLTSFQueue.hpp"
#include "utility/memory.hpp"

namespace warped {

/* Compares two events to see if one has a receive time less than to the other
 */
struct compareEvents {
public:
    bool operator() (const std::shared_ptr<Event>& first, 
                        const std::shared_ptr<Event>& second) const {

        bool is_less = false;
        if (first->timestamp() > second->timestamp()) {
            is_less = false;
        } else if (first->timestamp() == second->timestamp()) {
            if (first->rollback_cnt_ > second->rollback_cnt_) {
                is_less = false;
            } else if (first->rollback_cnt_ == second->rollback_cnt_) {
                is_less = (first->event_type_ < second->event_type_) ? true : false;
            } else {
                is_less = true;
            }
        } else {
            is_less = true;
        }
        return is_less;
    }
};

class TimeWarpEventSet {

public:
    TimeWarpEventSet() = default;

    void initialize (unsigned int num_of_objects, 
                     unsigned int num_of_schedulers,
                     unsigned int num_of_worker_threads);

    void insertEvent (unsigned int obj_id, std::shared_ptr<Event> event);

    std::shared_ptr<Event> getEvent (unsigned int thread_id);

    std::unique_ptr<std::vector<std::shared_ptr<Event>>> 
                            getEventsForCoastForward (unsigned int obj_id);

    void startScheduling (unsigned int obj_id);

    void replenishScheduler (unsigned int obj_id, std::shared_ptr<Event> old_event);

    void fossilCollectAll (unsigned int fossil_collect_time);

    void rollback (unsigned int obj_id, unsigned int rollback_time); 

private:
    //Number of simulation objects
    unsigned int num_of_objects_ = 0;

    //Lock to protect the unprocessed queues
    std::unique_ptr<std::mutex []> unprocessed_queue_lock_;

    //Lock to protect the unprocessed queues
    std::unique_ptr<bool []> is_unprocessed_queue_empty_;

    //Queues to hold the unprocessed events for each simulation object
    std::vector<std::unique_ptr<std::multiset<std::shared_ptr<Event>, 
                                        compareEvents>>> unprocessed_queue_;

    //Lock to protect the processed and coast forward queues
    std::unique_ptr<std::mutex []> processed_queue_lock_;

    //Queues to hold the processed events for each simulation object
    std::vector<std::unique_ptr<std::vector<std::shared_ptr<Event>>>> 
                                                        processed_queue_;

    //Queues to hold the events for coast forwarding of each simulation object
    std::vector<std::unique_ptr<std::vector<std::shared_ptr<Event>>>> 
                                                        coast_forward_queue_;

    //Number of event schedulers
    unsigned int num_of_schedulers_ = 0;

    //Lock to protect the schedule queues
    std::unique_ptr<std::mutex []> schedule_queue_lock_;

    //Queues to hold the scheduled events
    std::vector<std::unique_ptr<LTSFQueue>> schedule_queue_;

    //Map unprocessed queue to a schedule queue
    std::vector<unsigned int> unprocessed_queue_scheduler_map_;

    //Map worker thread to a schedule queue
    std::vector<unsigned int> worker_thread_scheduler_map_;

    //Rollback warning for objects
    std::vector<unsigned long long> rollback_warning_;

    //Count of GVT calculation request
    unsigned long long gvt_calc_request_cnt_ = 0;

    //Old rollback warning count
    unsigned int old_rollback_warning_cnt_ = 0;

    //New rollback warning count
    unsigned int new_rollback_warning_cnt_ = 0;
};

} // warped namespace

#endif
