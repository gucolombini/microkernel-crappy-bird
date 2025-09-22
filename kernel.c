/*
* Copyright (C) 2014  Arjun Sreedharan
* License: GPL version 2 or higher http://www.gnu.org/licenses/gpl.html
*/
#include "keyboard_map.h"

/* there are 25 lines each of 80 columns; each element takes 2 bytes */
#define LINES 25
#define COLUMNS_IN_LINE 80
#define BYTES_FOR_EACH_ELEMENT 2
#define SCREENSIZE BYTES_FOR_EACH_ELEMENT * COLUMNS_IN_LINE * LINES

#define KEYBOARD_DATA_PORT 0x60
#define KEYBOARD_STATUS_PORT 0x64
#define IDT_SIZE 256
#define INTERRUPT_GATE 0x8e
#define KERNEL_CODE_SEGMENT_OFFSET 0x08

#define ENTER_KEY_CODE 0x1C

#define TARGET_FPS 15
#define PIT_HZ 100  // 100 Hz timer

extern unsigned char keyboard_map[128];
extern void keyboard_handler(void); // para interrupts do teclado
extern void timer_handler(void);    // para função do update
extern char read_port(unsigned short port);
extern void write_port(unsigned short port, unsigned char data);
extern void load_idt(unsigned long *idt_ptr);

/* posição do cursor atual */
unsigned int current_loc = 0;

/* video memory begins at address 0xb8000 */
char *vidptr = (char*)0xb8000;

/* variáveis do jogo */
#define BIRD_X 5
#define PIPE_GAP 3
#define JUMP_SPEED -4
#define TERMINAL_VEL 2

#define SKY_COLOR 1
#define BIRD_COLOR 14
#define PIPE_COLOR 10

int bird_y = 5;
int bird_y_acceleration = JUMP_SPEED;

int pipe_a_x = 50;
int pipe_a_y = 8;
int pipe_b_x = 75;
int pipe_b_y = 9;

int score = 0;
int hiscore = 0;
int tick = 0;

/* build attribute byte */
static inline unsigned char vga_attr(unsigned char fg, unsigned char bg) {
    return (bg << 4) | (fg & 0x0F);
}

struct IDT_entry {
	unsigned short int offset_lowerbits;
	unsigned short int selector;
	unsigned char zero;
	unsigned char type_attr;
	unsigned short int offset_higherbits;
};

struct IDT_entry IDT[IDT_SIZE];


void idt_init(void)
{
	unsigned long keyboard_address;
	unsigned long idt_address;
	unsigned long idt_ptr[2];

	/* populate IDT entry of keyboard's interrupt */
	keyboard_address = (unsigned long)keyboard_handler;
	IDT[0x21].offset_lowerbits = keyboard_address & 0xffff;
	IDT[0x21].selector = KERNEL_CODE_SEGMENT_OFFSET;
	IDT[0x21].zero = 0;
	IDT[0x21].type_attr = INTERRUPT_GATE;
	IDT[0x21].offset_higherbits = (keyboard_address & 0xffff0000) >> 16;

    /* Timer IRQ (IRQ0 maps to INT 0x20 after PIC remap) */
    unsigned long timer_address = (unsigned long)timer_handler;
    IDT[0x20].offset_lowerbits = timer_address & 0xffff;
    IDT[0x20].selector = KERNEL_CODE_SEGMENT_OFFSET;
    IDT[0x20].zero = 0;
    IDT[0x20].type_attr = INTERRUPT_GATE;
    IDT[0x20].offset_higherbits = (timer_address & 0xffff0000) >> 16;


	/*     Ports
	*	 PIC1	PIC2
	*Command 0x20	0xA0
	*Data	 0x21	0xA1
	*/

	/* ICW1 - begin initialization */
	write_port(0x20 , 0x11);
	write_port(0xA0 , 0x11);

	/* ICW2 - remap offset address of IDT */
	/*
	* In x86 protected mode, we have to remap the PICs beyond 0x20 because
	* Intel have designated the first 32 interrupts as "reserved" for cpu exceptions
	*/
	write_port(0x21 , 0x20);
	write_port(0xA1 , 0x28);

	/* ICW3 - setup cascading */
	write_port(0x21 , 0x00);
	write_port(0xA1 , 0x00);

	/* ICW4 - environment info */
	write_port(0x21 , 0x01);
	write_port(0xA1 , 0x01);
	/* Initialization finished */

	/* mask interrupts */
	write_port(0x21 , 0xff);
	write_port(0xA1 , 0xff);

	/* fill the IDT descriptor */
	idt_address = (unsigned long)IDT ;
	idt_ptr[0] = (sizeof (struct IDT_entry) * IDT_SIZE) + ((idt_address & 0xffff) << 16);
	idt_ptr[1] = idt_address >> 16 ;

	load_idt(idt_ptr);
}

void kb_init(void)
{
	/* 0xFC = 11111100 -> enable IRQ0 (timer) and IRQ1 (keyboard) */
    write_port(0x21 , 0xFC);
}

void timer_init(int frequency) {
    int divisor = 1193182 / frequency;
    // command byte: channel 0, low/high, mode 3, binary
    write_port(0x43, 0x36);
    // divisor low then high
    write_port(0x40, divisor & 0xFF);
    write_port(0x40, (divisor >> 8) & 0xFF);
}

/* printa um string na posição do cursor atual na cor especificada */
void kprint(const char *str, unsigned char attr)
{
	unsigned int i = 0;
	while (str[i] != '\0') {
		vidptr[current_loc++] = str[i++];
		vidptr[current_loc++] = attr;
	}
}

void kprint_int(int value, unsigned char attr)
{
    char buf[12]; // buffer suficiente para um int 32 bit
    int i = 0;
    int is_negative = 0;

    if (value == 0) {
        vidptr[current_loc++] = '0';
        vidptr[current_loc++] = attr;
        return;
    }

    if (value < 0) {
        is_negative = 1;
        value = -value;
    }

    // converte int para string de casa por casa
    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }

    if (is_negative) {
        buf[i++] = '-';
    }

    // printa o buffer até onde o número chegou (ao contrário)
    for (int j = i - 1; j >= 0; j--) {
        vidptr[current_loc++] = buf[j];
        vidptr[current_loc++] = attr;
    }
}

void kprint_char(unsigned char c, unsigned char attr) {
    vidptr[current_loc++] = c;
    vidptr[current_loc++] = attr;
}

void kprint_newline(void)
{
	unsigned int line_size = BYTES_FOR_EACH_ELEMENT * COLUMNS_IN_LINE;
	current_loc = current_loc + (line_size - current_loc % (line_size));
}

/* move o cursor para a posição desejada */
void cursor_goto(int x, int y){
    unsigned int line_size = BYTES_FOR_EACH_ELEMENT * COLUMNS_IN_LINE;
    current_loc = (x*BYTES_FOR_EACH_ELEMENT) + (line_size * y);
}

/* funcao para desenhar o ceu, não só limpa a tela e pinta de azul mas também anima estrelas em paralaxe */
void clear_screen(void)
{
    unsigned int i = 0;
    unsigned char attr = vga_attr(15, SKY_COLOR); // céu azul :)
    while (i < SCREENSIZE) {
        if (((i+tick)/2)%34 == 1) { // efeito paralaxe para estrelas no céu
            vidptr[i++] = '.';
            vidptr[i++] = attr;
        } 
        else if (((i+tick/4)/2)%46 == 1) { // 2a camada de estrelas
            vidptr[i++] = '+';
            vidptr[i++] = attr;
        }
        else {
            vidptr[i++] = ' '; // céu padrão
            vidptr[i++] = attr;
        }
    }
}

void keyboard_handler_main(void)
{
	unsigned char status;
	char keycode;

	/* write EOI */
	write_port(0x20, 0x20);

	status = read_port(KEYBOARD_STATUS_PORT);
	/* Lowest bit of status will be set if buffer is not empty */
	if (status & 0x01) {
		keycode = read_port(KEYBOARD_DATA_PORT);
		if(keycode < 0)
			return;

		if(keycode == ENTER_KEY_CODE) {
			kprint_newline();
			return;
		}

		if(keyboard_map[(unsigned char) keycode] == 'w') {
            if (bird_y >= 99 + JUMP_SPEED*2) return;
            bird_y_acceleration = JUMP_SPEED;
        } else if (keyboard_map[(unsigned char) keycode] == 'r')
        {
            if (bird_y >= 99 + JUMP_SPEED*2) {
                restart_game();
            }
        }
        
		
	}
}

/* Basicamente a função de update do jogo */
void timer_handler_main(void) {
    static int frame_counter = 0;
    frame_counter++;

    if (frame_counter >= PIT_HZ / TARGET_FPS) {
        frame_counter = 0;

        clear_screen();
        bird_logic();
        make_bird(bird_y);
        move_pipe();
        make_pipe(pipe_a_x, pipe_a_y);
        make_pipe(pipe_b_x, pipe_b_y);
        misc_text_handler();
        tick++;
    }

    write_port(0x20, 0x20); // EOI
}

/* desenha o pássaro na posição especificada */
void make_bird(int y)
{
    cursor_goto(BIRD_X, y);
    if (y >= 0) {
        // asa abaixada ou levantada dependendo se o player acabou de pular ou não
        if (bird_y_acceleration <= JUMP_SPEED+2) kprint_char(0xDC, vga_attr(15, SKY_COLOR)); // asa batendo
        else kprint_char(0x5C, vga_attr(15, SKY_COLOR)); // asa abaixada
        kprint(" ", vga_attr(0, BIRD_COLOR)); // corpo
        kprint_char(0xF8, vga_attr(0, BIRD_COLOR)); // olho
        kprint_char(0x10, vga_attr(12, SKY_COLOR)); // bico
    }
}

/* lógica principal do pássaro */
void bird_logic() {
    if (bird_y >= 99) {
        cursor_goto(25, 5);
        kprint("VOCE PERDEU!!! HAHAHAHAHA!", vga_attr(12, 0));
        cursor_goto(23, 8);
        kprint("Pressione R para jogar novamente", vga_attr(14, 0));
        return;
    }
    if (pipe_a_x == BIRD_X) {
        if (bird_y - pipe_a_y > PIPE_GAP || bird_y - pipe_a_y < -PIPE_GAP) {
            bird_y = 99;
        } else {
            score ++;
        }
    }

    if (pipe_b_x == BIRD_X) {
        if (bird_y - pipe_b_y > PIPE_GAP || bird_y - pipe_b_y < -PIPE_GAP) {
            bird_y = 99;
        } else {
            score ++;
        }
    }
    
    if (bird_y_acceleration < TERMINAL_VEL) bird_y_acceleration += 1;
    bird_y += bird_y_acceleration;
}

/* desenha o cano na posição especificada */
void make_pipe(int x, int y) {
    int height = 20;
    char shade = 0xB1; // caractere para sombra do cano
    char light = 0xDD; // caractere para batendo no cano
    for (int i = 0; i < height; i++) {
        if (i == height -1) {
            cursor_goto(x-1, y-height-PIPE_GAP+i);
            kprint_char(shade, vga_attr(2, PIPE_COLOR));
            kprint("   ", vga_attr(15, PIPE_COLOR));
            kprint_char(light, vga_attr(15, PIPE_COLOR));
        } else {
            cursor_goto(x, y-height-PIPE_GAP+i);
            kprint_char(shade, vga_attr(2, PIPE_COLOR));
            kprint(" ", vga_attr(15, PIPE_COLOR));
            kprint_char(light, vga_attr(15, PIPE_COLOR));
        }
    }
    for (int i = 0; i < height; i++) {
        if (i == height -1) {
            cursor_goto(x-1, y+height+PIPE_GAP-i);
            kprint_char(shade, vga_attr(2, PIPE_COLOR));
            kprint("   ", vga_attr(15, PIPE_COLOR));
            kprint_char(light, vga_attr(15, PIPE_COLOR));
        } else {
            cursor_goto(x, y+height+PIPE_GAP-i);
            kprint_char(shade, vga_attr(2, PIPE_COLOR));
            kprint(" ", vga_attr(15, PIPE_COLOR));
            kprint_char(light, vga_attr(15, PIPE_COLOR));
        }
    }
}
/* move ambos os canos e reposiciona caso um atinja a borda da tela */
void move_pipe() {
    // posicionamento dos canos não é aleatório, ele apenas usa a altura atual do player e altera por um valor pré-definido
    // afinal, o input do player é mais aleatório do que qualquer função pseudorandômica hehehe..... né????????

    if (pipe_a_x > 0) {
        pipe_a_x -= 1;
    } else {
        pipe_a_x = 70;
        pipe_a_y = bird_y - 3;
        if (pipe_a_y < 3) {
            pipe_a_y = 4;
        }
    }

    if (pipe_b_x > 0) {
        pipe_b_x -= 1;
    } else {
        pipe_b_x = 70;
        pipe_b_y = bird_y - 1;
        if (pipe_b_y < 3) {
            pipe_b_y = 6;
        }
    }
}

/* exibe texto padrão e pontuação */
void misc_text_handler() {
    cursor_goto(0, 0);
    kprint("CRAPPY BIRD", vga_attr(SKY_COLOR, 15));
    kprint_newline();
    kprint("SCORE: ", vga_attr(SKY_COLOR, 15));
    kprint_int(score, vga_attr(SKY_COLOR, 15));
    if (hiscore > 0) {
        kprint_newline();
    kprint("HI SCORE: ", vga_attr(SKY_COLOR, 15));
    kprint_int(hiscore, vga_attr(SKY_COLOR, 15));
    }
}

void restart_game() {
    if (hiscore < score) hiscore = score;

    bird_y = 5;
    bird_y_acceleration = JUMP_SPEED;

    pipe_a_x = 50;
    pipe_a_y = 8;
    pipe_b_x = 75;
    pipe_b_y = 9;

    score = 0;
    tick = 0;
}


void kmain(void)
{

    idt_init();
    kb_init();
    timer_init(PIT_HZ);

    while(1) {
        asm volatile("hlt"); // halt até próximo interrupt
    }
}