/*
 * nn.h - mala biblioteka sieci neuronowych w czystym C (TCC64, gcc, ESP-IDF).
 *
 * Model sekwencyjny na tensorach HWC (wysokosc, szerokosc, kanaly - kanal najszybszy):
 *
 *     nn_model m;
 *     nn_init(&m, 49, 40, 1);                   // wejscie 49 x 40 x 1
 *     nn_conv(&m, 16, 3, 3, 2, 2, 1);           // conv 3x3, 16 kanalow, krok 2x2, ReLU
 *     nn_dwconv(&m, 3, 3, 1, 1, 1);             // depthwise 3x3
 *     nn_conv(&m, 32, 1, 1, 1, 1, 1);           // pointwise 1x1
 *     nn_gap(&m);                               // srednia globalna -> wektor
 *     nn_dense(&m, 2, 0);                       // warstwa w pelni polaczona -> logity
 *     nn_alloc(&m);                             // parametry
 *
 *     nn_ws ws; nn_ws_alloc(&ws, &m);
 *     const float *logits = nn_forward(&m, &ws, input);
 *
 * Trening (tylko PC): nn_train.h. Zapis/odczyt: nn_serialize / nn_deserialize.
 * Splot ma wypelnienie "same" (k/2), wagi: conv [oc][kh][kw][ic], depthwise [c][kh][kw],
 * dense [out][in].
 */
#ifndef NN_H
#define NN_H

#include <stddef.h>

#define NN_MAX_LAYERS 24

enum { NN_CONV = 1, NN_DWCONV = 2, NN_GAP = 3, NN_DENSE = 4 };

typedef struct {
    int type, relu;
    int kh, kw, sh, sw, ph, pw;
    int ih, iw, ic;               /* wejscie warstwy */
    int oh, ow, oc;               /* wyjscie warstwy */
    int nw, nb;                   /* liczba wag i biasow */
    int woff, boff;               /* polozenie w tablicy parametrow */
} nn_layer;

typedef struct {
    int      nl;
    nn_layer L[NN_MAX_LAYERS];
    int      ih, iw, ic;          /* wejscie sieci */
    int      nparams;
    float   *p;                   /* wszystkie parametry (wagi + biasy) */
    int      max_act;             /* najwieksza aktywacja (liczba float) */
    int      n_out;               /* rozmiar wyjscia */
    int      err;                 /* !=0 - blad budowy */
    int      own;                 /* 1 = p przydzielone przez nn_alloc (zwalnia nn_free) */
} nn_model;

typedef struct { float *a, *b; } nn_ws;   /* bufory robocze wnioskowania (2 x max_act) */

void  nn_init(nn_model *m, int ih, int iw, int ic);
int   nn_conv(nn_model *m, int oc, int kh, int kw, int sh, int sw, int relu);
int   nn_dwconv(nn_model *m, int kh, int kw, int sh, int sw, int relu);
int   nn_gap(nn_model *m);
int   nn_dense(nn_model *m, int n, int relu);
int   nn_alloc(nn_model *m);                 /* 0 = OK; parametry wyzerowane */
void  nn_free(nn_model *m);
long  nn_macs(const nn_model *m);            /* mnozenia na jedno wnioskowanie */
int   nn_in_size(const nn_model *m);

int   nn_ws_alloc(nn_ws *ws, const nn_model *m);
void  nn_ws_free(nn_ws *ws);

/* wnioskowanie: zwraca wskaznik na n_out wynikow (logitow) w buforze roboczym */
const float *nn_forward(const nn_model *m, nn_ws *ws, const float *in);
/* jedna warstwa (uzywa tez trener) */
void  nn_layer_forward(const nn_layer *L, const float *p, const float *in, float *out);
void  nn_softmax(float *x, int n);

/* zapis architektury + parametrow; buf == NULL -> zwraca potrzebny rozmiar */
size_t nn_serialize(const nn_model *m, void *buf, size_t cap);
/* odtworzenie z bufora; *used = zuzyte bajty. 0 = OK.
 * copy = 1: parametry kopiowane do pamieci; copy = 0: m->p wskazuje wprost do bufora
 * (np. partycja flash zmapowana w pamieci - zero RAM na wagi; bufor musi zyc, wyrownanie 4) */
int    nn_deserialize(nn_model *m, const void *buf, size_t len, size_t *used, int copy);

#endif /* NN_H */
