typedef union SDL_Event {
    unsigned int type;
    unsigned char padding[56];
} SDL_Event;

typedef struct EventHolder {
    unsigned char ready;
    SDL_Event event;
} EventHolder;
