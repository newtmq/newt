#include <newt/stomp.h>
#include <newt/common.h>
#include <newt/signal.h>
#include <newt/logger.h>
#include <newt/queue.h>
#include <newt/connection.h>
#include <newt/stomp_management_worker.h>

#include <assert.h>
#include <sys/ioctl.h>

#define RECV_BUFSIZE (4096)

struct stomp_frame_info {
  char *name;
  int len;
};
static struct stomp_frame_info finfo_arr[] = {
  {"SEND",        4},
  {"SUBSCRIBE",   9},
  {"CONNECT",     7},
  {"STOMP",       5},
  {"ACK",         3},
  {"BEGIN",       5},
  {"COMMIT",      6},
  {"ABORT",       5},
  {"NACK",        4},
  {"UNSUBSCRIBE", 11},
  {"DISCONNECT",  10},
  {0},
};

frame_bucket_t stomp_frame_bucket;

frame_t *alloc_frame() {
  frame_t *ret;

  ret = (frame_t *)malloc(sizeof(frame_t));
  if(ret == NULL) {
    return NULL;
  }

  /* Initialize frame_t object */
  memset(ret->name, 0, FNAME_LEN);
  memset(ret->id, 0, FRAME_ID_LEN);

  INIT_LIST_HEAD(&ret->h_attrs);
  INIT_LIST_HEAD(&ret->h_data);
  INIT_LIST_HEAD(&ret->l_bucket);
  INIT_LIST_HEAD(&ret->l_transaction);

  pthread_mutex_init(&ret->mutex_header, NULL);
  pthread_mutex_init(&ret->mutex_body, NULL);

  ret->sock = 0;
  ret->cinfo = NULL;
  ret->status = STATUS_BORN;
  ret->contentlen = -1;
  ret->has_contentlen = 0;

  ret->transaction_callback = NULL;
  ret->transaction_data = NULL;

  return ret;
}

void free_frame(frame_t *frame) {
  linedata_t *data, *l;

  /* delete header */
  if(! list_empty(&frame->h_attrs)) {
    list_for_each_entry_safe(data, l, &frame->h_attrs, list) {
      list_del(&data->list);
      free(data);
    }
  }

  /* delete body */
  if(! list_empty(&frame->h_data)) {
    list_for_each_entry_safe(data, l, &frame->h_data, list) {
      list_del(&data->list);
      free(data);
    }
  }

  pthread_mutex_lock(&stomp_frame_bucket.mutex);
  {
    if(frame->l_bucket.next != NULL && frame->l_bucket.prev != NULL) {
      list_del(&frame->l_bucket);
    }
  }
  pthread_mutex_unlock(&stomp_frame_bucket.mutex);

  if(frame->transaction_data != NULL) {
    free(frame->transaction_data);
  }

  free(frame);
}

static int ssplit(char *start, char *end, int *len) {
  char *p;
  int count = 0;
  int ret = RET_SUCCESS;

  assert(start != NULL);
  assert(end != NULL);

  if(IS_NL(start) || IS_BL(start)) {
    *len = 0;

    return RET_SUCCESS;
  }

  do {
    count++;
    p = start + count;

    if(p >= end) {
      debug("[ssplit] line is separated");
      ret = RET_ERROR;
    }
  } while(! (*p == '\0' || *p == '\n' || p >= end || count >= LD_MAX));

  *len = count;

  return ret;
}

static int frame_setname(char *data, int len, frame_t *frame) {
  int i, ret = RET_ERROR;
  struct stomp_frame_info *finfo;

  assert(frame != NULL);

  for(i=0; finfo=&finfo_arr[i], finfo!=NULL; i++) {
    if(len < finfo->len) {
      continue;
    }

    if(strncmp(data, finfo->name, finfo->len) == 0) {
      memcpy(frame->name, data, finfo->len);

      CLR(frame);
      SET(frame, STATUS_INPUT_HEADER);

      ret = RET_SUCCESS;
      break;
    }
  }

  return ret;
}

linedata_t *stomp_setdata(char *data, int len, struct list_head *head, pthread_mutex_t *lock) {
  linedata_t *attr;
  int offset = 0;
  int attrlen = len;

  do {
    if(attrlen > LD_MAX) {
      attrlen = LD_MAX;
    }

    debug("[stomp_setdata] (%d + %d) %s", len, offset, data);

    attr = (linedata_t *)malloc(sizeof(linedata_t));
    if(attr == NULL) {
      warn("failed to allocate linedata_t");
      return NULL;
    }

    memset(attr, 0, sizeof(linedata_t));
    memcpy(attr->data, data + offset, attrlen);
    INIT_LIST_HEAD(&attr->list);

    if(lock != NULL) {
      pthread_mutex_lock(lock);

      list_add_tail(&attr->list, head);

      pthread_mutex_unlock(lock);
    } else {
      list_add_tail(&attr->list, head);
    }

    offset += attrlen;
    attrlen = len - offset;
  } while(offset < len);

  return attr;
}

static int cleanup(void *data) {
  frame_t *frame;
  frame_t *h = NULL;

  pthread_mutex_lock(&stomp_frame_bucket.mutex);
  { /* thread safe */
    list_for_each_entry_safe(frame, h, &stomp_frame_bucket.h_frame, l_bucket) {
      free_frame(frame);
    }
  }
  pthread_mutex_unlock(&stomp_frame_bucket.mutex);

  return RET_SUCCESS;
}

static void frame_finish(frame_t *frame) {
  // last processing for current frame_t object
  CLR(frame);
  SET(frame, STATUS_IN_BUCKET);

  debug("(frame_finish) %s", frame->name);

  // set frame-id
  gen_random(frame->id, FRAME_ID_LEN);

  // correct broken header
  /*
  linedata_t *broken, *prev;
  broken = prev = NULL;
  pthread_mutex_lock(&frame->mutex_header);
  {
    linedata_t *header, *h;
    list_for_each_entry_safe(header, h, &frame->h_attrs, list) {
      debug("[frame_finish] (check) %s", header->data);
      if(broken != NULL) {
        sprintf(broken->data, "%s%s", broken->data, header->data);

        list_del(&header->list);
        free(header);

        debug("[frame_finish] (corrected) %s", broken->data);

        broken = NULL;
      } else if(index(header->data, ':') == NULL) {
        broken = header;

        debug("[frame_finish] (broken) %s", header->data);
      } else {
        prev = header;
      }
    }

    if(broken != NULL) {
      sprintf(prev->data, "%s%s", prev->data, broken->data);
      list_del(&broken->list);
      broken = NULL;

      debug("[frame_finish] (corrected) %s", prev->data);
    }
  }
  pthread_mutex_unlock(&frame->mutex_header);
  */

  pthread_mutex_lock(&stomp_frame_bucket.mutex);
  { /* thread safe */
    list_add_tail(&frame->l_bucket, &stomp_frame_bucket.h_frame);
  }
  pthread_mutex_unlock(&stomp_frame_bucket.mutex);
}

static int get_contentlen(char *input) {
  return atoi(input + 15);
}

static int is_contentlen(char *input, int len) {
  if(len > 15 && strncmp(input, "content-length:", 15) == 0) {
    return RET_SUCCESS;
  }
  return RET_ERROR;
}

static int frame_update(char *line, int len, stomp_conninfo_t *cinfo) {
  frame_t *frame = cinfo->frame;
  int ret = 0;
  assert(frame != NULL);

  if(GET(frame, STATUS_BORN)) {
    if(frame_setname(line, len, frame) == RET_ERROR) {
      warn("[frame_update] failed to set frame name");
    }
    debug("[frame_update] (%p) succeed in setting frame-name: %s", frame, frame->name);
  } else if(GET(frame, STATUS_INPUT_HEADER)) {
    if(IS_NL(line)) {
      if(frame->contentlen > 0) {
        CLR(frame);
        SET(frame, STATUS_INPUT_BODY);
        debug("[frame_update] succeed in separating header");

        return 0;
      } else if(frame->contentlen == 0) {
        debug("[frame_update] succeed in paring frame!! (zerobody)");
        frame_finish(frame);

        return 1;
      } else {
        debug("[frame_update] ignore head NL");
        while(! (IS_NL(line))) {
          line++;
          len -= 1;
        }
      }
    }

    if(len > 0) {
      if(is_contentlen(line, len) == RET_SUCCESS) {
        frame->contentlen = get_contentlen(line);
        debug("[frame_update] (%p) succeed in setting contentlen: %d", frame, frame->contentlen);
      }

      linedata_t *attr = stomp_setdata(line, len, &frame->h_attrs, &frame->mutex_header);
      if(attr != NULL) {
        if(cinfo->broken_attr != NULL) {
          sprintf(cinfo->broken_attr->data, "%s%s", cinfo->broken_attr->data, attr->data);

          list_del(&attr->list);
          free(attr);

          if(is_contentlen(line, len) == RET_SUCCESS) {
            frame->contentlen = get_contentlen(line);
            debug("[frame_update] (%p) succeed in setting contentlen: %d", frame, frame->contentlen);
          }

          debug("[frame_update] (%p) (corrected) %s", frame, cinfo->broken_attr->data);

          cinfo->broken_attr = NULL;

        } else if(index(line, ':') == NULL) {
          debug("[frame_update] (%p) (broken) %s", frame, attr->data);

          cinfo->broken_attr = attr;
        } else {
          cinfo->prev_attr = attr;
        }
      }
    }
  } else if(GET(frame, STATUS_INPUT_BODY)) {
    if(len > 0) {
      frame->has_contentlen += len;
      stomp_setdata(line, len, &frame->h_data, &frame->mutex_body);

      debug("[frame_update] (%p) succeed in set body [%d/%d]", frame, frame->contentlen, frame->has_contentlen);
    }

    if(IS_BL(line)) {
      warn("[frame_update] (%p) got blank-line [contentlen:%d/%d]", frame, frame->contentlen, frame->has_contentlen);
    }

    if(frame->contentlen <= frame->has_contentlen) {
      frame_finish(frame);
      debug("[frame_update] (%p) succeed in parsing frame!!", frame);

      ret = 1;
    }
  }

  return ret;
}

int stomp_init() {
  INIT_LIST_HEAD(&stomp_frame_bucket.h_frame);
  pthread_mutex_init(&stomp_frame_bucket.mutex, NULL);

  set_signal_handler(cleanup, NULL);

  return RET_SUCCESS;
}

static stomp_conninfo_t *conn_init() {
  stomp_conninfo_t *ret = NULL;

  ret = (stomp_conninfo_t *)malloc(sizeof(stomp_conninfo_t));
  if(ret != NULL) {
    memset(ret, 0, sizeof(stomp_conninfo_t));

    /* intiialize params */
    ret->status = 0;
    ret->prev_attr = NULL;
    ret->broken_attr = NULL;
    ret->prev_data = NULL;
    ret->prev_len = 0;
  }

  return ret;
}

static int conn_finish(void *data) {
  stomp_conninfo_t *cinfo = (stomp_conninfo_t *)data;
  int ret = RET_ERROR;

  if(cinfo != NULL) {
    frame_t *frame = cinfo->frame;

    if(frame != NULL && (GET(frame, STATUS_INPUT_BODY) || GET(frame, STATUS_INPUT_NAME))) {
      frame_finish(frame);

      ret = RET_SUCCESS;
    }
  }

  return ret;
}

static int cleanup_worker(void *data) {
  struct conninfo *cinfo = (struct conninfo *)data;
  int ret = RET_ERROR;

  if(cinfo != NULL) {
    close(cinfo->sock);

    if(cinfo->protocol_data != NULL) {
      free(cinfo->protocol_data);
    }

    free(cinfo);

    ret = RET_SUCCESS;
  }

  return ret;
}

void *stomp_conn_worker(struct conninfo *cinfo) {
  sighandle_t *handler;
  char buf[RECV_BUFSIZE];

  if(cinfo == NULL) {
    err("[connection_co_worker] thread argument is NULL");
    return NULL;
  }

  cinfo->protocol_data = (void *)conn_init();

  /* initialize processing after established connection */
  handler = set_signal_handler(cleanup_worker, &cinfo);

  int len;
  do {
    stomp_conninfo_t *stomp_cinfo = (stomp_conninfo_t *)cinfo->protocol_data;
    frame_t *frame = stomp_cinfo->frame;

    memset(buf, 0, RECV_BUFSIZE);

    len = recv(cinfo->sock, buf, sizeof(buf), 0);

    debug("[stomp_conn_worker] calling recv_data");

    recv_data(buf, len, cinfo->sock, cinfo->protocol_data);
  } while(len != 0);

  // cancel to parse of the current frame
  conn_finish(cinfo->protocol_data);

  del_signal_handler(handler);

  debug("[stomp_conn_worker] stopped");

  return NULL;
}

static void prepare_frame(stomp_conninfo_t *cinfo, int sock) {
  assert(cinfo != NULL);

  if(cinfo->frame == NULL) {
    frame_t *frame = alloc_frame(sock);

    assert(frame != NULL);

    /* initialize frame params */
    frame->sock = sock;
    frame->cinfo = cinfo;

    /* to reference this values over the 'recv_data' calling */
    cinfo->frame = frame;
  }
}

static int is_frame_name(const char *name, int len) {
  int i;
  struct stomp_frame_info *finfo;

  for(i=0; finfo=&finfo_arr[i], finfo!=NULL; i++) {
    if(len < finfo->len) {
      continue;
    } else if(finfo->len == 0) {
      break;
    }
    if(strncmp(name, finfo->name, finfo->len) == 0) {
      return RET_SUCCESS;
    }
  }

  return RET_ERROR;
}

int recv_data(char *recv_data, int len, int sock, void *_cinfo) {
  stomp_conninfo_t *cinfo = (stomp_conninfo_t *)_cinfo;
  char *curr, *next, *end, *line;

  curr = recv_data;
  end = (recv_data + len);

  debug("[recv_data] raw_data: %s [%d]", recv_data, len);

  while(curr < end) {
    int line_len, ret;
    int body_input = 0;

    debug("[recv_data] ====[%p/%p]====", curr, end);

    // in header parsing
    ret = ssplit(curr, end, &line_len);
    if(ret == RET_ERROR && is_frame_name(curr, line_len) == RET_ERROR) {
      debug("[recv_data] broken_line: %s [%d]", curr, line_len);

      char *data = malloc(LD_MAX);

      memset(data, 0, LD_MAX);
      strncpy(data, curr, line_len);

      cinfo->prev_data = data;
      cinfo->prev_len = line_len;

      break;
    }

    // set next position because following processing may change 'line_len' variable
    if(cinfo->frame != NULL && GET(cinfo->frame, STATUS_INPUT_BODY)) {
      next = curr + line_len;
      body_input = 1;
    } else {
      next = curr + line_len + 1;
    }

    if(cinfo->prev_data != NULL) {
      debug("[recv_data] (merge prev_data) '%s'(%d) + '%s'(%d)", cinfo->prev_data, cinfo->prev_len, curr, line_len);
      line = cinfo->prev_data;

      strncpy(line + cinfo->prev_len, curr, line_len);

      line_len += cinfo->prev_len;
      line[line_len] = '\0';
    } else {
      line = curr;
    }

    debug("[recv_data] correct_line: %s [%d]", line, line_len);

    if(cinfo->frame != NULL || line_len > 0) {
      prepare_frame(cinfo, sock);

      if(frame_update(line, line_len, cinfo) > 0) {
        cinfo->frame = NULL;
        if(body_input > 0) {
          next++;
        }
      }

      if(cinfo->frame != NULL) {
        debug("[recv_data] content [%d/%d]", cinfo->frame->contentlen, cinfo->frame->has_contentlen);
      }
    }

    if(cinfo->prev_data != NULL) {
      free(cinfo->prev_data);

      cinfo->prev_data = NULL;
      cinfo->prev_len = 0;
    }

    curr = next;
  }

  debug("[recv_data] finished");

  return RET_SUCCESS;
}

frame_t *get_frame_from_bucket() {
  frame_t *frame = NULL;

  pthread_mutex_lock(&stomp_frame_bucket.mutex);
  { /* thread safe */
    if(! list_empty(&stomp_frame_bucket.h_frame)) {
      frame = list_first_entry(&stomp_frame_bucket.h_frame, frame_t, l_bucket);
      list_del(&frame->l_bucket);
    }
  }
  pthread_mutex_unlock(&stomp_frame_bucket.mutex);

  return frame;
}

void stomp_send_error(int sock, char *body) {
  int i;
  char *msg[] = {
    "ERROR\n",
    "\n",
    body,
    NULL,
  };

  warn("(stomp_send_error) body: %s", body);

  if(is_socket_valid(sock) == RET_SUCCESS) {
    for(i=0; msg[i] != NULL; i++) {
      send_msg(sock, msg[i]);
    }
    send_msg(sock, NULL);
  }
}

void stomp_send_receipt(int sock, char *id) {
  int i;
  char buf[LD_MAX] = {0};

  if(sock > 0 && id != NULL) {
    sprintf(buf, "receipt-id:%s\n", id);

    send_msg(sock, "RECEIPT\n");
    send_msg(sock, buf);
    send_msg(sock, NULL);
  }
}

void stomp_send_message(int sock, frame_t *frame, struct list_head *headers) {
  linedata_t *header;
  linedata_t *body;

  send_msg(sock, "MESSAGE\n");

  list_for_each_entry(header, &frame->h_attrs, list) {
    send_msg(sock, header->data);
    send_msg(sock, "\n");
  }

  if(headers != NULL) {
    list_for_each_entry(header, headers, list) {
      send_msg(sock, header->data);
      send_msg(sock, "\n");
    }
  }

  // send static headers
  {
    send_msg(sock, "message-id: ");
    send_msg(sock, frame->id);
    send_msg(sock, "\n");
  }

  if(! list_empty(&frame->h_data)) {
    // separation of headers and body
    send_msg(sock, "\n");

    list_for_each_entry(body, &frame->h_data, list) {
      send_msg(sock, body->data);
    }
  }

  send_msg(sock, NULL);
}
