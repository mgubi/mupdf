// Multi-threaded rendering of all pages in a document to PNG images.

// First look at doc/example.c and make sure you understand it.
// Then read the multi-threading section in doc/overview.txt,
// before coming back here to see an example of multi-threading.

// This example will create one main thread for reading pages from the
// document, and one thread per page for rendering. After rendering
// the main thread will wait for each rendering thread to complete before
// writing that thread's rendered image to a PNG image. There is
// nothing in MuPDF requiring a rendering thread to only render a
// single page, this is just a design decision taken for this example.

// Compile a debug build of mupdf, then compile and run this example:
//
// gcc -g -o build/debug/example-mt -Iinclude doc/multi-threading.c \
//	build/debug/lib*.a -lpthread -lm
//
// build/debug/example-mt /path/to/document.pdf
//
// Caution! As all pages are rendered simultaneously, please choose a
// file with just a few pages to avoid stressing your machine too
// much. Also you may run in to a limitation on the number of threads
// depending on your environment.

// Include the MuPDF header file, and pthread's header file.
#include <mupdf/fitz.h>
#include <pthread.h>

// A convenience function for dying abruptly on pthread errors.

void
fail(char *msg)
{
	fprintf(stderr, "%s", msg);
	abort();
}

// The data structure passed between the requesting main thread and
// each rendering thread.

struct data {
	// A pointer to the original context in the main thread sent
	// from main to rendering thread. It will be used to create
	// each rendering thread's context clone.
	fz_context *ctx;

	// Page number sent from main to rendering thread for printing
	int pagenumber;

	// The display list as obtained by the main thread and sent
	// from main to rendering thread. This contains the drawing
	// commands (text, images, etc.) for the page that should be
	// rendered.
	fz_display_list *list;

	// The area of the page to render as obtained by the main
	// thread and sent from main to rendering thread.
	fz_rect bbox;

	// This is the result, a pixmap containing the rendered page.
	// It is passed first from main thread to the rendering
	// thread, then its samples are changed by the rendering
	// thread, and then back from the rendering thread to the main
	// thread.
	fz_pixmap *pix;
};

// This is the function run by each rendering function. It takes
// pointer to an instance of the data structure described above and
// renders the display list into the pixmap before exiting.

void *
renderer(void *data)
{
	int pagenumber = ((struct data *) data)->pagenumber;
	fz_context *ctx = ((struct data *) data)->ctx;
	fz_display_list *list = ((struct data *) data)->list;
	fz_rect bbox = ((struct data *) data)->bbox;
	fz_pixmap *pix = ((struct data *) data)->pix;

	fprintf(stderr, "thread at page %d loading!\n", pagenumber);

	// The context pointer is pointing to the main thread's
	// context, so here we create a new context based on it for
	// use in this thread.

	ctx = fz_clone_context(ctx);

	// Next we run the display list through the draw device which
	// will render the request area of the page to the pixmap.

	fprintf(stderr, "thread at page %d rendering!\n", pagenumber);
	fz_device *dev = fz_new_draw_device(ctx, pix);
	fz_run_display_list(list, dev, &fz_identity, &bbox, NULL);
	fz_free_device(dev);

	// This threads context is freed.

	fz_free_context(ctx);

	fprintf(stderr, "thread at page %d done!\n", pagenumber);

	return data;
}

// These are the two locking functions required by MuPDF when
// operating in a multi-threaded environment. They each take a user
// argument that can be used to transfer some state, in this case a
// pointer to the array of mutexes.

void lock_mutex(void *user, int lock)
{
	pthread_mutex_t *mutex = (pthread_mutex_t *) user;

	if (pthread_mutex_lock(&mutex[lock]) != 0)
		fail("pthread_mutex_lock()");
}

void unlock_mutex(void *user, int lock)
{
	pthread_mutex_t *mutex = (pthread_mutex_t *) user;

	if (pthread_mutex_unlock(&mutex[lock]) != 0)
		fail("pthread_mutex_unlock()");
}

int main(int argc, char **argv)
{
	char *filename = argv[1];
	pthread_t *thread = NULL;
	fz_locks_context locks;
	pthread_mutex_t mutex[FZ_LOCK_MAX];
	int i;

	// Initialize FZ_LOCK_MAX number of non-recursive mutexes.

	for (i = 0; i < FZ_LOCK_MAX; i++)
	{
		if (pthread_mutex_init(&mutex[i], NULL) != 0)
			fail("pthread_mutex_init()");
	}

	// Initialize the locking structure with function pointers to
	// the locking functions and to the user data. In this case
	// the user data is a pointer to the array of mutexes so the
	// locking functions can find the relevant lock to change when
	// they are called. This way we avoid global variables.

	locks.user = mutex;
	locks.lock = lock_mutex;
	locks.unlock = unlock_mutex;

	// This is the main threads context function, so supply the
	// locking structure. This context will be used to parse all
	// the pages from the document.

	fz_context *ctx = fz_new_context(NULL, &locks, FZ_STORE_UNLIMITED);

	// Open the PDF, XPS or CBZ document.

	fz_document *doc = fz_open_document(ctx, filename);

	// Retrieve the number of pages, which translates to the
	// number of threads used for rendering pages.

	int threads = fz_count_pages(doc);
	fprintf(stderr, "spawning %d threads, one per page...\n", threads);

	thread = malloc(threads * sizeof (pthread_t));

	for (i = 0; i < threads; i++)
	{
		// Load the relevant page for each thread.

		fz_page *page = fz_load_page(doc, i);

		// Compute the bounding box for each page.

		fz_rect bbox;
		fz_irect rbox;
		fz_bound_page(doc, page, &bbox);

		// Create a display list that will hold the drawing
		// commands for the page.

		fz_display_list *list = fz_new_display_list(ctx);

		// Run the loaded page through a display list device
		// to populate the page's display list.

		fz_device *dev = fz_new_list_device(ctx, list);
		fz_run_page(doc, page, dev, &fz_identity, NULL);
		fz_free_device(dev);

		// The page is no longer needed, all drawing commands
		// are now in the display list.

		fz_free_page(doc, page);

		// Create a white pixmap using the correct dimensions.

		fz_pixmap *pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), fz_round_rect(&rbox, &bbox));
		fz_clear_pixmap_with_value(ctx, pix, 0xff);

		// Populate the data structure to be sent to the
		// rendering thread for this page.

		struct data *data = malloc(sizeof (struct data));

		data->pagenumber = i + 1;
		data->ctx = ctx;
		data->list = list;
		data->bbox = bbox;
		data->pix = pix;

		// Create the thread and pass it the data structure.

		if (pthread_create(&thread[i], NULL, renderer, data) != 0)
			fail("pthread_create()");
	}

	// Now each thread is rendering pages, so wait for each thread
	// to complete its rendering.

	fprintf(stderr, "joining %d threads...\n", threads);
	for (i = threads - 1; i >= 0; i--)
	{
		char filename[42];
		struct data *data;

		if (pthread_join(thread[i], (void **) &data) != 0)
			fail("pthread_join");

		sprintf(filename, "out%04d.png", i);
		fprintf(stderr, "\tSaving %s...\n", filename);

		// Write the rendered image to a PNG file

		fz_write_png(ctx, data->pix, filename, 0);

		// Free the thread's pixmap and display list since
		// they were allocated by the main thread above.

		fz_drop_pixmap(ctx, data->pix);
		fz_free_display_list(ctx, data->list);

		// Free the data structured passed back and forth
		// between the main thread and rendering thread.

		free(data);
	}

	fprintf(stderr, "finally!\n");
	fflush(NULL);

	free(thread);

	// Finally the document is closed and the main thread's
	// context is freed.

	fz_close_document(doc);
	fz_free_context(ctx);

	return 0;
}
