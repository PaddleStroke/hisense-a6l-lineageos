#include <assert.h>
#include <stdio.h>
#include <poll.h>
typedef unsigned int __poll_t;
#define EPOLLIN POLLIN
#define EPOLLRDNORM POLLRDNORM
struct iio_event_interface { int wait; int det_events; };
struct iio_dev_opaque { struct iio_event_interface *event_interface; };
struct iio_dev { void *info; struct iio_dev_opaque opaque; };
struct file { struct iio_dev *private_data; };
struct poll_table_struct { int unused; };
#define to_iio_dev_opaque(d) (&(d)->opaque)
#define kfifo_is_empty(f) (*(f) == 0)
#define poll_wait(f,w,p) ((void)0)
static __poll_t iio_event_poll(struct file *filep,
			     struct poll_table_struct *wait)
{
	struct iio_dev *indio_dev = filep->private_data;
	struct iio_dev_opaque *iio_dev_opaque = to_iio_dev_opaque(indio_dev);
	struct iio_event_interface *ev_int = iio_dev_opaque->event_interface;
	__poll_t events = 0;

	if (!indio_dev->info)
		return events;

	poll_wait(filep, &ev_int->wait, wait);

	if (!kfifo_is_empty(&ev_int->det_events))
		events = EPOLLIN | EPOLLRDNORM;

	return events;
}
int main(void) {
    struct iio_event_interface ev = {0,1};
    struct iio_dev d = {(void*)1, {&ev}}; struct file f = {&d};
    assert(iio_event_poll(&f, NULL) == (POLLIN | POLLRDNORM));
    d.info = NULL;
    assert(iio_event_poll(&f, NULL) == 0);
    puts("IIO_UNREGISTERED poll_mask=0 even_with_queued_event (no_HUP_no_ERR_no_IN)");
}
