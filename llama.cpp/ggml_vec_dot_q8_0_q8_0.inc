static void ggml_vec_dot_q8_0_q8_0(const int n, float * restrict s, const void * restrict vx, const void * restrict vy) {
    const int qk = QK8_0;
    const int nb = n / qk;

    assert(n % qk == 0);

    const block_q8_0 * restrict x = vx;
    const block_q8_0 * restrict y = vy;

#if defined(__ARM_NEON)
    float32x4_t sumv0 = vdupq_n_f32(0.0f);
    float32x4_t sumv1 = vdupq_n_f32(0.0f);

    assert(nb % 2 == 0); // TODO: handle odd nb

    for (int i = 0; i < nb; i += 2) {
        const block_q8_0 * restrict x0 = &x[i + 0];
        const block_q8_0 * restrict x1 = &x[i + 1];
        const block_q8_0 * restrict y0 = &y[i + 0];
        const block_q8_0 * restrict y1 = &y[i + 1];

        const int8x16_t x0_0 = vld1q_s8(x0->qs);
        const int8x16_t x0_1 = vld1q_s8(x0->qs + 16);
        const int8x16_t x1_0 = vld1q_s8(x1->qs);
        const int8x16_t x1_1 = vld1q_s8(x1->qs + 16);

        // load y
        const int8x16_t y0_0 = vld1q_s8(y0->qs);
        const int8x16_t y0_1 = vld1q_s8(y0->qs + 16);
        const int8x16_t y1_0 = vld1q_s8(y1->qs);
        const int8x16_t y1_1 = vld1q_s8(y1->qs + 16);

#if defined(__ARM_FEATURE_DOTPROD)
        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), x0_0, y0_0),
                        vdotq_s32(vdupq_n_s32(0), x0_1, y0_1))), GGML_FP16_TO_FP32(x0->d)*GGML_FP16_TO_FP32(y0->d));

        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(
                        vdotq_s32(vdupq_n_s32(0), x1_0, y1_0),
                        vdotq_s32(vdupq_n_s32(0), x1_1, y1_1))), GGML_FP16_TO_FP32(x1->d)*GGML_FP16_TO_FP32(y1->d));

#else
        const int16x8_t p0_0 = vmull_s8(vget_low_s8 (x0_0), vget_low_s8 (y0_0));
        const int16x8_t p0_1 = vmull_s8(vget_high_s8(x0_0), vget_high_s8(y0_0));
        const int16x8_t p0_2 = vmull_s8(vget_low_s8 (x0_1), vget_low_s8 (y0_1));
        const int16x8_t p0_3 = vmull_s8(vget_high_s8(x0_1), vget_high_s8(y0_1));

        const int16x8_t p1_0 = vmull_s8(vget_low_s8 (x1_0), vget_low_s8 (y1_0));
        const int16x8_t p1_1 = vmull_s8(vget_high_s8(x1_0), vget_high_s8(y1_0));
        const int16x8_t p1_2 = vmull_s8(vget_low_s8 (x1_1), vget_low_s8 (y1_1));
        const int16x8_t p1_3 = vmull_s8(vget_high_s8(x1_1), vget_high_s8(y1_1));

        const int32x4_t p0 = vaddq_s32(vpaddlq_s16(p0_0), vpaddlq_s16(p0_1));
        const int32x4_t p1 = vaddq_s32(vpaddlq_s16(p0_2), vpaddlq_s16(p0_3));
        const int32x4_t p2 = vaddq_s32(vpaddlq_s16(p1_0), vpaddlq_s16(p1_1));
        const int32x4_t p3 = vaddq_s32(vpaddlq_s16(p1_2), vpaddlq_s16(p1_3));

        sumv0 = vmlaq_n_f32(sumv0, vcvtq_f32_s32(vaddq_s32(p0, p1)), GGML_FP16_TO_FP32(x0->d)*GGML_FP16_TO_FP32(y0->d));
        sumv1 = vmlaq_n_f32(sumv1, vcvtq_f32_s32(vaddq_s32(p2, p3)), GGML_FP16_TO_FP32(x1->d)*GGML_FP16_TO_FP32(y1->d));
#endif
    }

    *s = vaddvq_f32(sumv0) + vaddvq_f32(sumv1);
#elif defined(__AVX2__) || defined(__AVX__)
    // Initialize accumulator with zeros
    __m256 acc = _mm256_setzero_ps();

    // Main loop
    for (int i = 0; i < nb; ++i) {
        // Compute combined scale for the block
        const __m256 d = _mm256_set1_ps(GGML_FP16_TO_FP32(x[i].d) * GGML_FP16_TO_FP32(y[i].d));
        __m256i bx = _mm256_loadu_si256((const __m256i *)x[i].qs);
        __m256i by = _mm256_loadu_si256((const __m256i *)y[i].qs);

#if defined(__AVX2__)
        const __m256 q = mul_sum_i8_pairs_float_avx2(bx, by);
        // Multiply q with scale and accumulate
        acc = _mm256_fmadd_ps( d, q, acc );
#else
        const __m256 q = mul_sum_i8_pairs_float_avx(bx, by);
        // Multiply q with scale and accumulate
        acc = _mm256_add_ps( _mm256_mul_ps( d, q ), acc );
#endif
    }

    *s = hsum_float_8(acc);
#elif defined(__riscv_v_intrinsic)
    float sumf = 0.0;
    size_t vl = __riscv_vsetvl_e8m1(qk);

    for (int i = 0; i < nb; i++) {
        // load elements
        vint8m1_t bx = __riscv_vle8_v_i8m1(x[i].qs, vl);
        vint8m1_t by = __riscv_vle8_v_i8m1(y[i].qs, vl);

        vint16m2_t vw_mul = __riscv_vwmul_vv_i16m2(bx, by, vl);

        vint32m1_t v_zero = __riscv_vmv_v_x_i32m1(0, vl);
        vint32m1_t v_sum = __riscv_vwredsum_vs_i16m2_i32m1(vw_mul, v_zero, vl);

        int sumi = __riscv_vmv_x_s_i32m1_i32(v_sum);

        sumf += sumi*(GGML_FP16_TO_FP32(x[i].d)*GGML_FP16_TO_FP32(y[i].d));
    }

    *s = sumf;
#else
    // scalar
    float sumf = 0.0;

    for (int i = 0; i < nb; i++) {
        int sumi = 0;

        for (int j = 0; j < qk; j++) {
            sumi += x[i].qs[j]*y[i].qs[j];
        }

        sumf += sumi*(GGML_FP16_TO_FP32(x[i].d)*GGML_FP16_TO_FP32(y[i].d));
    }

    *s = sumf;
#endif
}
