#ifndef TAMISU_FILE_WRAPPER_H
#define TAMISU_FILE_WRAPPER_H

#include <linux/file.h>
#include <linux/fs.h>

int tamisu_install_file_wrapper(int fd);
void tamisu_file_wrapper_init(void);

#endif // #ifndef TAMISU_FILE_WRAPPER_H
