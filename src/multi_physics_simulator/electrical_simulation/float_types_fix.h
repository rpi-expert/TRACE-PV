// Workaround for CUDA compilation with newer GCC versions
// Disable IEC 60559 extended float types before system headers
#ifndef FLOAT_TYPES_FIX_H
#define FLOAT_TYPES_FIX_H

// Disable the feature that enables _Float types
#ifndef __STDC_WANT_IEC_60559_TYPES_EXT__
#define __STDC_WANT_IEC_60559_TYPES_EXT__ 0
#endif

#endif // FLOAT_TYPES_FIX_H

