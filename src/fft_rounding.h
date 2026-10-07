/* Arithmetic recovered from ttpcomm 5.7.9, 6000F790 and 600104B0.
 * Only the forward 256-complex/512-real float path calls these helpers.
 * Products of float operands fit in double; explicit float stores reproduce
 * the original x87 stack spills without depending on compiler register choice.
 */
#ifndef TTPCOMM_FFT_ROUNDING_H
#define TTPCOMM_FFT_ROUNDING_H
static float ttp_fft_float(double value) {
    volatile float result = (float)value;
    return result;
}
static void ttp_fft_bfly4(kiss_fft_cpx* out, size_t stride,
                          const kiss_fft_cpx* twiddle, size_t m) {
    size_t k;
    for (k = 0; k < m; ++k, ++out) {
        const kiss_fft_cpx a = out[m], b = out[2*m], c = out[3*m];
        const kiss_fft_cpx w1 = twiddle[k*stride], w2 = twiddle[k*stride*2], w3 = twiddle[k*stride*3];
        const double ar = (double)a.r*w1.r - (double)a.i*w1.i;
        const double ai = (double)a.i*w1.r + (double)a.r*w1.i;
        const double br = (double)b.r*w2.r - (double)b.i*w2.i;
        const double bi = (double)b.i*w2.r + (double)b.r*w2.i;
        const float cr = ttp_fft_float((double)c.r*w3.r - (double)c.i*w3.i);
        const float ci = ttp_fft_float((double)c.i*w3.r + (double)c.r*w3.i);
        const float dr = ttp_fft_float(out->r - br), di = ttp_fft_float(out->i - bi);
        const float sr = ttp_fft_float(out->r + br), si = ttp_fft_float(out->i + bi);
        const float acr = ttp_fft_float(ar + cr), aci = ttp_fft_float(ai + ci);
        const float dcr = ttp_fft_float(ar - cr);
        const double dci = ai - ci;
        out[2*m].r = ttp_fft_float((double)sr - acr);
        out[2*m].i = ttp_fft_float((double)si - aci);
        out->r = ttp_fft_float((double)sr + acr);
        out->i = ttp_fft_float((double)si + aci);
        out[m].r = ttp_fft_float(dr + dci);
        out[m].i = ttp_fft_float((double)di - dcr);
        out[3*m].r = ttp_fft_float(dr - dci);
        out[3*m].i = ttp_fft_float((double)di + dcr);
    }
}
static void ttp_fft_unpack(kiss_fft_cpx a, kiss_fft_cpx b, kiss_fft_cpx w,
                           kiss_fft_cpx* low, kiss_fft_cpx* high) {
    const float sr = ttp_fft_float((double)a.r+b.r), si = ttp_fft_float((double)a.i+b.i);
    const float dr = ttp_fft_float((double)a.r-b.r);
    const double di = (double)a.i-b.i;
    const double tr = (double)dr*w.r-di*w.i;
    const float ti = ttp_fft_float((double)dr*w.i+di*w.r);
    low->r = ttp_fft_float((sr+tr)*0.5);
    low->i = ttp_fft_float(((double)si+ti)*0.5);
    high->r = ttp_fft_float((sr-tr)*0.5);
    high->i = ttp_fft_float(((double)si-ti)*-0.5);
}
#endif
