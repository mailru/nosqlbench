
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#include "nosqlbench.h"

extern struct nb nb;

extern volatile sig_atomic_t nb_signaled;

static pthread_mutex_t lock_stats = PTHREAD_MUTEX_INITIALIZER;

static void
nb_worker_account(struct nb_worker *worker, int batch)
{
	struct nb_stat *current = nb_history_current(&worker->history);
	int missed = 0;
	worker->db.dif->recv(&worker->db, batch, &missed);
	current->cnt_miss += missed;

	nb_history_account(&worker->history, batch);
	nb_history_avg(&worker->history);
	pthread_mutex_lock(&lock_stats);
	nb_statistics_set(&nb.stats, worker->id, &worker->history.Savg);
	pthread_mutex_unlock(&lock_stats);
}

static void *nb_worker(void *ptr)
{
	struct nb_worker *worker = ptr;

	nb.db->init(&worker->db, 1);
	if (nb.db->connect(&worker->db, nb.opts.host, nb.opts.port) == -1)
		return NULL;

	while (!nb.is_done) {
		nb_workload_reset(&worker->workload);
		nb_history_start(&worker->history);

		struct nb_request *req;
		while ((req = nb_workload_fetch(&worker->workload)) && !nb.is_done) {
			/* generating key and making request */
			worker->key->generate(&worker->keyv, worker->workload.count);
			req->_do(&worker->db, &worker->keyv);
			req->requested++;
			worker->workload.requested++;
			nb_history_add(&worker->history, req->type == NB_SELECT, 1);

			/* in case of 'delete' request, we have to
			 * reinsert delete entry */
			if (req->type == NB_DELETE) {
				nb.db->insert(&worker->db, &worker->keyv);
				worker->workload.requested++;
				nb_history_add(&worker->history, 0, 1);
			}

			/* processing requests in request_batch_count intervals */
			if (worker->workload.requested % nb.opts.request_batch_count == 0 &&
			    worker->workload.requested > 0) {
				worker->workload.processed += nb.opts.request_batch_count;
				nb_worker_account(worker, nb.opts.request_batch_count);
			}
		}

		int still_to_process = worker->workload.requested -
			               worker->workload.processed;
		if (still_to_process > 0)
			nb_worker_account(worker, still_to_process);
	}
	return NULL;
}

static void nb_create(void)
{
	int i;
	for (i = 0; i < nb.opts.threads_start; i++)
		nb_workers_create(&nb.workers,
				  nb.db,
				  nb.key,
				  nb.key_dist,
				  &nb.workload, 
				  nb.opts.history_per_batch,
				  nb_worker);
}

static void nb_create_step(void)
{
	if (nb.opts.threads_policy == NB_THREADS_ATONCE)
		return;
	if (nb.workers.count < nb.opts.threads_max) {
		int bottom = 0;
		int top = 0;
		if (nb.workers.count + nb.opts.threads_increment > nb.opts.threads_max)
			top = nb.opts.threads_max - nb.workers.count;
		else
			top = nb.opts.threads_increment;
		for (; bottom < top; bottom++)
			nb_workers_create(&nb.workers,
					  nb.db,
					  nb.key,
					  nb.key_dist,
					  &nb.workload, 
					  nb.opts.history_per_batch,
					  nb_worker);

		pthread_mutex_lock(&lock_stats);
		nb_statistics_resize(&nb.stats, nb.workers.count);
		pthread_mutex_unlock(&lock_stats);
	}
}

static void nb_report(void)
{
	pthread_mutex_lock(&lock_stats);
	nb_statistics_report(&nb.stats, nb.workers.count, nb.tick);
	nb.report->report();
	pthread_mutex_unlock(&lock_stats);
}


static void nb_limit(void)
{
	if (nb_signaled) {
		nb.is_done = 1;
		return;
	}
	switch (nb.opts.benchmark_policy) {
	case NB_BENCHMARK_NOLIMIT:
		return;
	case NB_BENCHMARK_THREADLIMIT:
		if (nb.workers.count == nb.opts.threads_max)
			nb.is_done = 1;
		break;
	case NB_BENCHMARK_TIMELIMIT:
		if (nb_period_equal(nb.opts.time_limit))
			nb.is_done = 1;
		break;
	case NB_BENCHMARK_REQLIMIT:
		break;
	}
}

void nb_engine(void)
{
	nb_create();

	while (!nb.is_done) {
		sleep(1);
		nb_limit();

		if (nb_period_equal(nb.opts.report_interval))
			nb_report();

		if (nb_period_equal(nb.opts.threads_interval))
			nb_create_step();

		nb_tick();
	}

	nb_workers_join(&nb.workers);

	nb_statistics_final(&nb.stats);
	nb.report->report_final();

	if (nb.opts.csv_file)
		nb_statistics_csv(&nb.stats, nb.opts.csv_file);

	pthread_mutex_destroy(&lock_stats);
}
