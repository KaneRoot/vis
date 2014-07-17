#define _BSD_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "editor.h"

#define MAX(a, b)  ((a) < (b) ? (b) : (a))

#define BUFFER_SIZE (1 << 20)

/* Buffer holding the file content, either readonly mmap(2)-ed from the original
 * file or heap allocated to store the modifications.
 */
typedef struct Buffer Buffer;
struct Buffer {
	size_t size;            /* maximal capacity */
	size_t pos;             /* current insertion position */
	char *content;          /* actual data */
	Buffer *next;           /* next junk */
};

/* A piece holds a reference (but doesn't itself store) a certain amount of data.
 * All active pieces chained together form the whole content of the document.
 * At the beginning there exists only one piece, spanning the whole document.
 * Upon insertion/delition new pieces will be created to represent the changes.
 * Generally pieces are never destroyed, but kept around to peform undo/redo operations.
 */
typedef struct Piece Piece;
struct Piece {
	Editor *editor;                   /* editor to which this piece belongs */
	Piece *prev, *next;               /* pointers to the logical predecessor/successor */
	Piece *global_prev, *global_next; /* double linked list in order of allocation, used to free individual pieces */
	char *content;                    /* pointer into a Buffer holding the data */
	size_t len;                       /* the lenght in number of bytes starting from content */
	// size_t line_count;
	int index;                        /* unique index identifiying the piece */
};

typedef struct {
	Piece *piece;
	size_t off;
} Location;

/* A Span holds a certain range of pieces. Changes to the document are allways
 * performed by swapping out an existing span with a new one.
 */
typedef struct {
	Piece *start, *end;     /* start/end of the span */
	size_t len;             /* the sum of the lenghts of the pieces which form this span */
	// size_t line_count;
} Span;

/* A Change keeps all needed information to redo/undo an insertion/deletion. */
typedef struct Change Change;
struct Change {
	Span old;               /* all pieces which are being modified/swapped out by the change */
	Span new;               /* all pieces which are introduced/swapped in by the change */
	Change *next;
};

/* An Action is a list of Changes which are used to undo/redo all modifications
 * since the last snapshot operation. Actions are kept in an undo and a redo stack.
 */
typedef struct Action Action;
struct Action {
	Change *change;         /* the most recent change */
	Action *next;           /* next action in the undo/redo stack */
	time_t time;            /* when the first change of this action was performed */
};

/* The main struct holding all information of a given file */
struct Editor {
	Buffer buf;             /* original mmap(2)-ed file content at the time of load operation */
	Buffer *buffers;        /* all buffers which have been allocated to hold insertion data */
	Piece *pieces;		/* all pieces which have been allocated, used to free them */
	int piece_count;	/* number of pieces allocated, only used for debuging purposes */
	Piece begin, end;       /* sentinel nodes which always exists but don't hold any data */
	Action *redo, *undo;    /* two stacks holding all actions performed to the file */
	Action *current_action; /* action holding all file changes until a snapshot is performed */
	Action *saved_action;   /* the last action at the time of the save operation */
	size_t size;            /* current file content size in bytes */
	const char *filename;   /* filename of which data was loaded */
	struct stat info;	/* stat as proped on load time */
	int fd;                 /* the file descriptor of the original mmap-ed data */
};

/* prototypes */
static Buffer *buffer_alloc(Editor *ed, size_t size);
static void buffer_free(Buffer *buf);
static char *buffer_store(Editor *ed, char *content, size_t len);
static Piece *piece_alloc(Editor *ed);
static void piece_free(Piece *p);
static void piece_init(Piece *p, Piece *prev, Piece *next, char *content, size_t len);
static void span_init(Span *span, Piece *start, Piece *end);
static void span_swap(Editor *ed, Span *old, Span *new);
static Change *change_alloc(Editor *ed);
static void change_free(Change *c);
static Action *action_alloc(Editor *ed);
static void action_free(Action *a);
static void action_push(Action **stack, Action *action);
static Action *action_pop(Action **stack);

static Buffer *buffer_alloc(Editor *ed, size_t size) {
	Buffer *buf = calloc(1, sizeof(Buffer));
	if (!buf)
		return NULL;
	if (BUFFER_SIZE > size)
		size = BUFFER_SIZE;
	if (!(buf->content = malloc(size))) {
		free(buf);
		return NULL;
	}
	buf->size = size;
	buf->next = ed->buffers;
	ed->buffers = buf;
	return buf;
}

static void buffer_free(Buffer *buf) {
	if (!buf)
		return;
	free(buf->content);
	free(buf);
}

static char *buffer_store(Editor *ed, char *content, size_t len) {
	Buffer *buf = ed->buffers;
	if (!buf) {
		if (!(buf = buffer_alloc(ed, len)))
			return NULL;
	}
	size_t n = buf->size - buf->pos;
	if (n < len) {
		/* not engough space in this buffer, allocate a new one.
		 * this waste some space we could also split the piece
		 * inorder to fill the buffer
		 */
		if (!(buf = buffer_alloc(ed, len)))
			return NULL;
	} else {
		n = len;
	}
	char *dest = memcpy(buf->content + buf->pos, content, len);
	buf->pos += len;
	return dest;
}

static void span_init(Span *span, Piece *start, Piece *end) {
	size_t len = 0;
	span->start = start;
	span->end = end;
	for (Piece *p = start; p; p = p->next) {
		len += p->len;
		if (p == end)
			break;
	}
	span->len = len;
}

static void span_swap(Editor *ed, Span *old, Span *new) {
	/* TODO use a balanced search tree to keep the pieces
		instead of a doubly linked list.
	 */
	if (old->len == 0 && new->len == 0) {
		return;
	} else if (old->len == 0) {
		/* insert new span */
		new->start->prev->next = new->start;
		new->end->next->prev = new->end;
	} else if (new->len == 0) {
		/* delete old span */
		old->start->prev->next = old->end->next;
		old->end->next->prev = old->start->prev;
	} else {
		/* replace old with new */
		old->start->prev->next = new->start;
		old->end->next->prev = new->end;
	}
	ed->size -= old->len;
	ed->size += new->len;
}

static void action_push(Action **stack, Action *action) {
	action->next = *stack;
	*stack = action;
}

static Action *action_pop(Action **stack) {
	Action *action = *stack;
	if (action)
		*stack = action->next;
	return action;
}

static Action *action_alloc(Editor *ed) {
	Action *old, *new = calloc(1, sizeof(Action));
	if (!new)
		return NULL;
	new->time = time(NULL);
	/* throw a away all old redo operations, since we are about to perform a new one */
	while ((old = action_pop(&ed->redo)))
		action_free(old);
	ed->current_action = new;
	action_push(&ed->undo, new);
	return new;
}

static void action_free(Action *a) {
	if (!a)
		return;
	for (Change *next, *c = a->change; c; c = next) {
		next = c->next;
		change_free(c);
	}
	free(a);
}

static Piece *piece_alloc(Editor *ed) {
	Piece *p = calloc(1, sizeof(Piece));
	if (!p)
		return NULL;
	p->editor = ed;
	p->index = ++ed->piece_count;
	p->global_next = ed->pieces;
	if (ed->pieces)
		ed->pieces->global_prev = p;
	ed->pieces = p;
	return p;
}

static void piece_free(Piece *p) {
	if (!p)
		return;
	if (p->global_prev)
		p->global_prev->global_next = p->global_next;
	if (p->global_next)
		p->global_next->global_prev = p->global_prev;
	if (p->editor->pieces == p)
		p->editor->pieces = p->global_next;
	free(p);
}

static void piece_init(Piece *p, Piece *prev, Piece *next, char *content, size_t len) {
	p->prev = prev;
	p->next = next;
	p->content = content;
	p->len = len;
}

static Location piece_get(Editor *ed, size_t pos) {
	Location loc = {};
	// TODO: handle position at end of file: pos+1
	size_t cur = 0;
	for (Piece *p = &ed->begin; p->next; p = p->next) {
		if (cur <= pos && pos <= cur + p->len) {
			loc.piece = p;
			loc.off = pos - cur;
			return loc;
		}
		cur += p->len;
	}

	return loc;
}

static Change *change_alloc(Editor *ed) {
	Action *a = ed->current_action;
	if (!a) {
		a = action_alloc(ed);
		if (!a)
			return NULL;
	}
	Change *c = calloc(1, sizeof(Change));
	if (!c)
		return NULL;
	c->next = a->change;
	a->change = c;
	return c;
}

static void change_free(Change *c) {
	/* only free the new part of the span, the old one is still in use */
	piece_free(c->new.start);
	if (c->new.start != c->new.end)
		piece_free(c->new.end);
	free(c);
}

static Piece* editor_insert_empty(Editor *ed, char *content, size_t len) {
	Piece *p = piece_alloc(ed);
	if (!p)
		return NULL;
	piece_init(&ed->begin, NULL, p, NULL, 0);
	piece_init(p, &ed->begin, &ed->end, content, len);
	piece_init(&ed->end, p, NULL, NULL, 0);
	ed->size = len;
	return p;
}

bool editor_insert(Editor *ed, size_t pos, char *text) {
	Change *c = change_alloc(ed);
	if (!c)
		return false;
	size_t len = strlen(text); // TODO
	if (!(text = buffer_store(ed, text, len)))
		return false;
	/* special case for an empty document */
	if (ed->size == 0) {
		Piece *p = editor_insert_empty(ed, text, len);
		if (!p)
			return false;
		span_init(&c->new, p, p);
		span_init(&c->old, NULL, NULL);
		return true;
	}
	Location loc = piece_get(ed, pos);
	Piece *p = loc.piece;
	size_t off = loc.off;
	if (off == p->len) {
		/* insert between two existing pieces */
		Piece *new = piece_alloc(ed);
		if (!new)
			return false;
		piece_init(new, p, p->next, text, len);
		span_init(&c->new, new, new);
		span_init(&c->old, NULL, NULL);
	} else {
		/* insert into middle of an existing piece, therfore split the old
		 * piece. that is we have 3 new pieces one containing the content
		 * before the insertion point then one holding the newly inserted
		 * text and one holding the content after the insertion point.
		 */
		Piece *before = piece_alloc(ed);
		Piece *new = piece_alloc(ed);
		Piece *after = piece_alloc(ed);
		if (!before || !new || !after)
			return false;

		// TODO: check index calculation
		piece_init(before, p->prev, new, p->content, off);
		piece_init(new, before, after, text, len);
		piece_init(after, new, p->next, p->content + off, p->len - off);

		span_init(&c->new, before, after);
		span_init(&c->old, p, p);
	}

	span_swap(ed, &c->old, &c->new);
	return true;
}

bool editor_undo(Editor *ed) {
	Action *a = action_pop(&ed->undo);
	if (!a)
		return false;
	for (Change *c = a->change; c; c = c->next) {
		span_swap(ed, &c->new, &c->old);
	}

	action_push(&ed->redo, a);
	return true;
}

bool editor_redo(Editor *ed) {
	Action *a = action_pop(&ed->redo);
	if (!a)
		return false;
	for (Change *c = a->change; c; c = c->next) {
		span_swap(ed, &c->old, &c->new);
	}

	action_push(&ed->undo, a);
	return true;
}

bool copy_content(void *data, size_t pos, const char *content, size_t len) {
	char **p = (char **)data;
	memcpy(*p, content, len);
	*p += len;
	return true;
}

int editor_save(Editor *ed, const char *filename) {
	size_t len = strlen(filename) + 10;
	char tmpname[len];
	snprintf(tmpname, len, ".%s.tmp", filename);
	// TODO file ownership, permissions etc
	/* O_RDWR is needed because otherwise we can't map with MAP_SHARED */
	int fd = open(tmpname, O_CREAT|O_RDWR|O_TRUNC, S_IRUSR|S_IWUSR);
	if (fd == -1)
		return -1;
	if (ftruncate(fd, ed->size) == -1)
		goto err;
	if (ed->size > 0) {
		void *buf = mmap(NULL, ed->size, PROT_WRITE, MAP_SHARED, fd, 0);
		if (buf == MAP_FAILED)
			goto err;

		void *cur = buf;
		editor_iterate(ed, &cur, 0, copy_content);

		if (munmap(buf, ed->size) == -1)
			goto err;
	}
	if (close(fd) == -1)
		return -1;
	if (rename(tmpname, filename) == -1)
		return -1;
	ed->saved_action = ed->undo;
	editor_snapshot(ed);
err:
	close(fd);
	return -1;
}

Editor *editor_load(const char *filename) {
	Editor *ed = calloc(1, sizeof(Editor));
	if (!ed)
		return NULL;
	ed->begin.index = 1;
	ed->end.index = 2;
	ed->piece_count = 2;
	piece_init(&ed->begin, NULL, &ed->end, NULL, 0);
	piece_init(&ed->end, &ed->begin, NULL, NULL, 0);
	if (filename) {
		ed->filename = filename;
		ed->fd = open(filename, O_RDONLY);
		if (ed->fd == -1)
			goto out;
		if (fstat(ed->fd, &ed->info) == -1)
			goto out;
		if (!S_ISREG(ed->info.st_mode))
			goto out;
		// XXX: use lseek(fd, 0, SEEK_END); instead?
		ed->buf.size = ed->info.st_size;
		ed->buf.content = mmap(NULL, ed->info.st_size, PROT_READ, MAP_SHARED, ed->fd, 0);
		if (ed->buf.content == MAP_FAILED)
			goto out;
		if (!editor_insert_empty(ed, ed->buf.content, ed->buf.size))
			goto out;
	}
	return ed;
out:
	if (ed->fd > 2)
		close(ed->fd);
	editor_free(ed);
	return NULL;
}

static void print_piece(Piece *p) {
	fprintf(stderr, "index: %d\tnext: %d\tprev: %d\t len: %d\t content: %p\n", p->index,
		p->next ? p->next->index : -1,
		p->prev ? p->prev->index : -1,
		p->len, p->content);
	fflush(stderr);
	write(1, p->content, p->len);
	write(1, "\n", 1);
}

void editor_debug(Editor *ed) {
	for (Piece *p = &ed->begin; p; p = p->next) {
		print_piece(p);
	}
}

void editor_iterate(Editor *ed, void *data, size_t pos, iterator_callback_t callback) {
	Location loc = piece_get(ed, pos);
	Piece *p = loc.piece;
	if (!p)
		return;
	size_t len = p->len - loc.off;
	char *content = p->content + loc.off;
	while (p && callback(data, pos, content, len)) {
		pos += len;
		p = p->next;
		if (!p)
			return;
		content = p->content;
		len = p->len;
	}
}

bool editor_delete(Editor *ed, size_t pos, size_t len) {
	if (len == 0)
		return true;
	if (pos + len > ed->size)
		return false;
	Location loc = piece_get(ed, pos);
	Piece *p = loc.piece;
	size_t off = loc.off;
	size_t cur; // how much has already been deleted
	bool midway_start = false, midway_end = false;
	Change *c = change_alloc(ed);
	if (!c)
		return false;
	Piece *before, *after; // unmodified pieces before / after deletion point
	Piece *start, *end; // span which is removed
	if (off == p->len) {
		/* deletion starts at a piece boundry */
		cur = 0;
		before = p;
		start = p->next;
	} else {
		/* deletion starts midway through a piece */
		midway_start = true;
		cur = p->len - off;
		start = p;
		before = piece_alloc(ed);
	}
	/* skip all pieces which fall into deletion range */
	while (cur < len) {
		p = p->next;
		cur += p->len;
	}

	if (cur == len) {
		/* deletion stops at a piece boundry */
		end = p;
		after = p->next;
	} else { // cur > len
		/* deletion stops midway through a piece */
		midway_end = true;
		end = p;
		after = piece_alloc(ed);
		piece_init(after, before, p->next, p->content + p->len - (cur - len), cur - len);
	}

	if (midway_start) {
		/* we finally now which piece follows our newly allocated before piece */
		piece_init(before, start->prev, after, start->content, off);
	}

	Piece *new_start = NULL, *new_end = NULL;
	if (midway_start) {
		new_start = before;
		if (!midway_end)
			new_end = before;
	}
	if (midway_end) {
		if (!midway_start)
			new_start = after;
		new_end = after;
	}

	span_init(&c->new, new_start, new_end);
	span_init(&c->old, start, end);
	span_swap(ed, &c->old, &c->new);
	return true;
}

bool editor_replace(Editor *ed, size_t pos, char *c) {
	// TODO argument validation: pos etc.
	size_t len = strlen(c);
	editor_delete(ed, pos, len);
	editor_insert(ed, pos, c);
	return true;
}

void editor_snapshot(Editor *ed) {
	ed->current_action = NULL;
}

void editor_free(Editor *ed) {
	if (!ed)
		return;

	Action *a;
	while ((a = action_pop(&ed->undo)))
		action_free(a);
	while ((a = action_pop(&ed->redo)))
		action_free(a);

	for (Piece *next, *p = ed->pieces; p; p = next) {
		next = p->global_next;
		piece_free(p);
	}

	for (Buffer *next, *buf = ed->buffers; buf; buf = next) {
		next = buf->next;
		buffer_free(buf);
	}

	if (ed->buf.content)
		munmap(ed->buf.content, ed->buf.size);

	free(ed);
}

bool editor_modified(Editor *ed) {
	return ed->saved_action != ed->undo;
}
