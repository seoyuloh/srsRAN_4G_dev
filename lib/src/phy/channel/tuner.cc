#include "srsran/phy/channel/tuner.h"
#include "srsran/phy/utils/vector.h"

void srsran_channel_tuner_execute(srsran_channel_tuner_t*   q,
                                  const cf_t*               in,
                                  cf_t*                     out,
                                  uint32_t                  nsamples)
{
  srsran_vec_sc_prod_cfc(in, q->tuner_attenuation, out, nsamples);
}

