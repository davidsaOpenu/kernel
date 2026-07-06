#pragma once

struct exofs_submodule {
    int (*init)(void);
    void (*exit)(void);
};
