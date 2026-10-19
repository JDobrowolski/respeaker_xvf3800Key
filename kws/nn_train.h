/*
 * nn_train.h - trening sieci z nn.h na PC (TCC64, Windows): propagacja wsteczna,
 * AdamW, entropia krzyzowa z softmax, rownolegle na wszystkich rdzeniach.
 *
 *     nn_trainer *t = nn_trainer_new(&m, 0);           // 0 = wszystkie rdzenie
 *     nn_he_init(&m, 1234);
 *     for (...) {
 *         float loss = nn_train_step(t, X, y, n, lr, 1e-4f, &ok);   // X: n x nn_in_size
 *     }
 *     nn_predict(t, X, n, probs);                      // probs: n x n_out (softmax)
 */
#ifndef NN_TRAIN_H
#define NN_TRAIN_H

#include "../components/kws/nn.h"

typedef struct nn_trainer nn_trainer;
typedef void (*nn_job)(void *ctx, int i0, int i1, int thread);

nn_trainer *nn_trainer_new(nn_model *m, int threads);
void  nn_trainer_free(nn_trainer *t);
int   nn_threads(const nn_trainer *t);

void  nn_he_init(nn_model *m, unsigned seed);

/* jeden krok AdamW na paczce; zwraca srednia strate, *correct = trafienia argmax */
float nn_train_step(nn_trainer *t, const float *X, const int *y, int n, float lr, float wd, int *correct);
void  nn_predict(nn_trainer *t, const float *X, int n, float *probs);

/* rownolegla petla na watkach trenera: fn(ctx, i0, i1, nr_watku) dla [0,n) */
void  nn_parallel(nn_trainer *t, nn_job fn, void *ctx, int n);

#endif /* NN_TRAIN_H */
