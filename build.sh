#!/bin/sh
gcc -Wall -g test_app.c superqueue.c -o test_app -lpthread -lssl -lcrypto
