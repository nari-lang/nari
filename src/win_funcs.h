// functions that are only needed on windows
// mostly because they are posix functions that don't exist on windows.

#pragma once

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#ifdef _WIN32
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define GETLINE_MINSIZE 16
static int getline(char **lineptr, size_t *n, FILE *fp) {
  int ch;
  int i = 0;
  char free_on_err = 0;
  char *p;

  errno = 0;
  if (lineptr == NULL || n == NULL || fp == NULL) {
    errno = EINVAL;
    return -1;
  }
  if (*lineptr == NULL) {
    *n = GETLINE_MINSIZE;
    *lineptr = (char *)malloc(sizeof(char) * (*n));
    if (*lineptr == NULL) {
      errno = ENOMEM;
      return -1;
    }
    free_on_err = 1;
  }

  for (i = 0;; i++) {
    ch = fgetc(fp);
    while (i >= (*n) - 2) {
      *n *= 2;
      p = (char *)realloc(*lineptr, sizeof(char) * (*n));
      if (p == NULL) {
        if (free_on_err)
          free(*lineptr);
        errno = ENOMEM;
        return -1;
      }
      *lineptr = p;
    }
    if (ch == EOF) {
      if (i == 0) {
        if (free_on_err)
          free(*lineptr);
        return -1;
      }
      (*lineptr)[i] = '\0';
      *n = i;
      return i;
    }

    if (ch == '\n') {
      (*lineptr)[i] = '\n';
      (*lineptr)[i + 1] = '\0';
      *n = i + 1;
      return i + 1;
    }
    (*lineptr)[i] = (char)ch;
  }
}
#endif
