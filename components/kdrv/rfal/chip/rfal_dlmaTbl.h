/**
  ******************************************************************************
  * @file    rfal_dpoTbl.h
  * @author  MMY Application Team
  * @brief   RF Dynamic Power Table default values
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#ifndef ST25R3916_DLMA_TBL_H
#define ST25R3916_DLMA_TBL_H

/*
 ******************************************************************************
 * INCLUDES
 ******************************************************************************
 */
#include "rfal_dlma.h"


/*
 ******************************************************************************
 * GLOBAL DATA TYPES
 ******************************************************************************
 */
 
/*! Default DLMA table */
/*  PRQA S 3674 2 # CERT ARR02 - Flexible array will be used with sizeof, on adding elements error-prone manual update of size would be required */
/*  PRQA S 3406 1 # MISRA 8.6 - Externally generated table included by the library */   /*  PRQA S 1514 1 # MISRA 8.9 - Externally generated table included by the library */
const rfalDlmaEntry rfalDlmaDefaultSettings [] = {
    {0x0F, 0x01,  39,   0},
    {0x0F, 0x03,  69,  40},
    {0x0F, 0x04,  79,  70},
    {0x0F, 0x05, 255,  80}
};

#endif /* ST25R3916_DLMA_TBL_H */
