#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>

#include <newt/common.h>
#include <newt/list.h>
#include <newt/queue.h>
#include <newt/logger.h>

struct list_head *queuebox[QB_SIZE] = {0};
static pthread_mutex_t queuebox_lock;

static unsigned long hash(unsigned char *str) {
  unsigned long hash = 5381;
  int c;
  
  while (c = *str++) {
    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  }
  
  return hash;
}

static struct q_entry *create_entry(void *data) {
  struct q_entry *e;

  e = (struct q_entry*)malloc(sizeof(struct q_entry));
  if(e != NULL) {
    e->data = data;
    INIT_LIST_HEAD(&e->l_queue);
  }

  return e;
}

static struct queue *create_queue() {
  struct queue *q;
  
  q = (struct queue*)malloc(sizeof(struct queue));
  if(q != NULL) {
    q->hashnum = 0;
    q->count = 0;

    pthread_mutex_init(&q->mutex, NULL);

    INIT_LIST_HEAD(&q->l_box);
    INIT_LIST_HEAD(&q->h_entry);

    memset(q->name, 0, QNAME_LENGTH);
  }

  return q;
}

static void clear_entry(struct q_entry *e) {
  if(e != NULL) {
    list_del(&e->l_queue);
    free(e);
  }
}

static void clear_queue(struct queue* q) {
  struct q_entry *entry, *e;

  if(q != NULL) {
    list_for_each_entry_safe(entry, e, &q->h_entry, l_queue) {
      clear_entry(entry);
    }
    if(! list_empty(&q->l_box)) {
      list_del(&q->l_box);
    }
    free(q);
  }
}

static struct queue *get_queue(char *qname) {
  unsigned long hashnum = hash(qname);
  int index = (hashnum % QB_SIZE) & INT_MAX;
  struct queue *queue = NULL;

  /* create new_queue*/
  pthread_mutex_lock(&queuebox_lock);
  if(queuebox[index] == NULL) {
    struct list_head *head = (struct list_head *)malloc(sizeof(struct list_head));
    if(head == NULL) {
      return NULL;
    }
    INIT_LIST_HEAD(head);

    queuebox[index] = head;
  }
  pthread_mutex_unlock(&queuebox_lock);

  /* get queue */
  struct queue *q;
  pthread_mutex_lock(&queuebox_lock);
  list_for_each_entry(q, queuebox[index], l_box) {
    if(q->hashnum == hashnum) {
      queue = q;
    }
  }
  pthread_mutex_unlock(&queuebox_lock);

  if(queue == NULL) {
    /* target queue doesn't exist yet */
    queue = create_queue();
    if(queue != NULL) {
      queue->hashnum = hashnum;

      // set qname
      int namelen = strlen(qname);
      if(namelen > QNAME_LENGTH) {
        namelen = QNAME_LENGTH;
      }
      memcpy(queue->name, qname, namelen);

      pthread_mutex_lock(&queuebox_lock);
      {
        list_add(&queue->l_box, queuebox[index]);
      }
      pthread_mutex_unlock(&queuebox_lock);
    }
  }

  return queue;
}

int enqueue(void *data, char *qname) {
  struct q_entry* e;
  struct queue *q;

  if(data == NULL) {
    return RET_ERROR;
  }

  e = create_entry(data);
  if(e == NULL) {
    return RET_ERROR;
  }

  q = get_queue(qname);
  if(q == NULL) {
    clear_entry(e);
    return RET_ERROR;
  }

  /* set entry to queue */
  pthread_mutex_lock(&q->mutex);
  {
    list_add_tail(&e->l_queue, &q->h_entry);
    q->count += 1;
  }
  pthread_mutex_unlock(&q->mutex);

  return RET_SUCCESS;
}

void *dequeue(char *qname) {
  struct q_entry *e = NULL;
  struct queue *q;
  void *ret = NULL;

  q = get_queue(qname);
  if(q == NULL) {
    return NULL;
  }

  pthread_mutex_lock(&q->mutex);
  {
    if(! list_empty(&q->h_entry)) {
      e = list_first_entry(&q->h_entry, struct q_entry, l_queue);
      list_del(&e->l_queue);

      ret = e->data;
    }
  }
  pthread_mutex_unlock(&q->mutex);

  return ret;
}

int get_queuelist(struct list_head *head) {
  int i, count = 0;

  assert(head != NULL);

  INIT_LIST_HEAD(head);

  for(i=0; i<QB_SIZE; i++) {
    if(queuebox[i] != NULL) {
      struct queue *queue;

      list_for_each_entry(queue, queuebox[i], l_box) {
        struct q_entry *entry = (struct q_entry *)malloc(sizeof(struct q_entry));
        if(entry != NULL) {
          INIT_LIST_HEAD(&entry->l_queue);
          list_add_tail(&entry->l_queue, head);

          entry->data = (void *)queue;

          count++;
        }
      }
    }
  }

  return count;
}

int cleanup_queuebox() {
  int i;

  pthread_mutex_lock(&queuebox_lock);
  {
    struct queue *queue, *q;

    for(i=0; i<QB_SIZE; i++) {
      if(queuebox[i] == NULL || list_empty(queuebox[i])) {
        continue;
      }

      list_for_each_entry_safe(queue, q, queuebox[i], l_box) {
        clear_queue(queue);
      }
    }
  }
  pthread_mutex_unlock(&queuebox_lock);

  return RET_SUCCESS;
}

int initialize_queuebox() {
  memset(queuebox, 0, sizeof(queuebox));
  pthread_mutex_init(&queuebox_lock, NULL);

  return RET_SUCCESS;
}
