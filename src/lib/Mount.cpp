//--------------------------------------------------------------------------------------------------
// telescope mount control
#include <Arduino.h>
#include "../../Constants.h"
#include "../../Config.h"
#include "../../ConfigX.h"
#include "../HAL/HAL.h"
#include "../pinmaps/Models.h"
#include "../debug/Debug.h"
#include "../tasks/OnTask.h"
extern Tasks tasks;

#if AXIS1_DRIVER_MODEL != OFF && AXIS2_DRIVER_MODEL != OFF

#include "../coordinates/Convert.h"
extern Convert convert;
#include "../coordinates/Transform.h"
extern Transform transform;
#include "../commands/ProcessCmds.h"
extern GeneralErrors generalErrors;
#include "../StepDrivers/StepDrivers.h"
#include "Axis.h"
extern Axis axis1;
extern AxisSettings axis1Settings;
extern Axis axis2;
extern AxisSettings axis2Settings;
#include "Clock.h"
extern Clock clock;
#include "Mount.h"

void Mount::init(int8_t mountType) {
  VF("MSG: Mount::init, type "); VL(mountType);
  this->mountType = mountType;
  if (mountType == GEM) meridianFlip = MF_ALWAYS;

  radsPerCentisecond = degToRad(15.0/3600.0)/100.0;

  // setup axis1
  axis1.init(1, axis1Settings);
  axis1.setInstrumentCoordinate(degToRad(90.0));
  axis1.enable(true);

  // setup axis2
  axis2.init(2, axis2Settings);
  axis2.setInstrumentCoordinate(degToRad(90.0));
  axis2.enable(true);

  // ------------------------------------------------------------------------------------------------
  // move in measures (radians) per second, tracking_enabled
  VLF("MSG: Mount::init, starting tracking");
  trackingState = TS_SIDEREAL;
  axis1.setFrequencyMax(degToRad(4.0));
  axis1.setTracking(true);
  trackingRate = hzToSidereal(SIDEREAL_RATE_HZ);
  updateTrackingRates();
}

bool Mount::command(char reply[], char command[], char parameter[], bool *supressFrame, bool *numericReply, CommandError *commandError) {
  char *conv_end;
  PrecisionMode precisionMode = convert.precision;

  //  C - Sync Control
  // :CS#       Synchonize the telescope with the current right ascension and declination coordinates
  //            Returns: Nothing (Sync's fail silently)
  // :CM#       Synchonize the telescope with the current database object (as above)
  //            Returns: "N/A#" on success, "En#" on failure where n is the error code per the :MS# command
  if (cmd("CS") || cmd("CM")) {
    CommandError e;
    //if (alignActive()) { e = alignStar(); if (e != CE_NONE) { alignNumStars = 0; alignThisStar = 0; commandError = e; } } else e = syncEqu(gotoTarget);
    e = syncEqu(&gotoTarget);
    if (command[1] == 'M') {
      if (e >= CE_GOTO_ERR_BELOW_HORIZON && e <= CE_GOTO_ERR_UNSPECIFIED) strcpy(reply,"E0"); reply[1] = (char)(e - CE_GOTO_ERR_BELOW_HORIZON) + '1';
      if (e == CE_NONE) strcpy(reply,"N/A");
    }
    *numericReply = false;
  } else

  // :MS#       Goto the Target Object
  //            Returns:
  //              0=Goto is possible
  //              1=below the horizon limit
  //              2=above overhead limit
  //              3=controller in standby
  //              4=mount is parked
  //              5=Goto in progress
  //              6=outside limits (AXIS2_LIMIT_MAX, AXIS2_LIMIT_MIN, AXIS1_LIMIT_MIN/MAX, MERIDIAN_E/W)
  //              7=hardware fault
  //              8=already in motion
  //              9=unspecified error
  if (cmd("MS"))  {
    CommandError e = gotoEqu(&gotoTarget);
    strcpy(reply,"0");
    if (e >= CE_GOTO_ERR_BELOW_HORIZON && e <= CE_GOTO_ERR_UNSPECIFIED) reply[0] = (char)(e - CE_GOTO_ERR_BELOW_HORIZON) + '1';
    if (e == CE_NONE) reply[0] = '0';
    *numericReply = false;
    *supressFrame = true;
    *commandError = e;
  } else

  // :GA#       Get Mount Altitude
  //            Returns: sDD*MM# or sDD*MM'SS# (based on precision setting)
  // :GAH#      High precision
  //            Returns: sDD*MM'SS.SSS# (high precision)
  if (cmdH("GA")) {
    updatePosition();
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToDms(reply, radToDeg(transform.mountToNative(&current, true).a), false, true, precisionMode);
    *numericReply = false;
  } else

  // :Ga#       Get Target Altitude
  //            Returns: sDD*MM# or sDD*MM'SS# (based on precision setting)
  // :GaH#      High precision
  //            Returns: sDD*MM'SS.SSS# (high precision)
  if (cmdH("Ga")) {
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToHms(reply, radToDeg(gotoTarget.a), false, precisionMode);
    *numericReply = false;
  } else

  // :GD#       Get Mount Declination
  //            Returns: sDD*MM# or sDD*MM:SS# (based on precision setting)
  // :GDH#      Returns: sDD*MM:SS.SSS# (high precision)
  if (cmdH("GD")) {
    updatePosition();
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToDms(reply, radToDeg(transform.mountToNative(&current).d), false, true, precisionMode);
    *numericReply = false;
  } else

  // :Gd#       Get Target Declination
  //            Returns: HH:MM.T# or HH:MM:SS (based on precision setting)
  // :GdH#      High precision
  //            Returns: HH:MM:SS.SSS# (high precision)
  if (cmdH("Gd")) {
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToDms(reply, radToDeg(gotoTarget.d), false, true, precisionMode);
    *numericReply = false;
  } else

  // :Gh#       Get Horizon Limit, the minimum elevation of the mount relative to the horizon
  //            Returns: sDD*#
  if (cmd("Gh"))  { sprintf(reply,"%+02ld*", lround(radToDeg(limits.minAltitude))); *numericReply = false; } else

  // :Go#       Get Overhead Limit
  //            Returns: DD*#
  //            The highest elevation above the horizon that the telescope will goto
  if (cmd("Go"))  { sprintf(reply,"%02ld*", lround(radToDeg(limits.maxAltitude))); *numericReply=false; } else

  // :GR#       Get Mount Right Ascension
  //            Returns: HH:MM.T# or HH:MM:SS# (based on precision setting)
  // :GRH#      Returns: HH:MM:SS.SSSS# (high precision)
  if (cmdH("GR")) {
    updatePosition();
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToHms(reply, radToHrs(transform.mountToNative(&current).r), false, precisionMode);
    *numericReply = false;
  } else

  // :Gr#       Get Target Right Ascension
  //            Returns: HH:MM.T# or HH:MM:SS (based on precision setting)
  // :GrH#      Returns: HH:MM:SS.SSSS# (high precision)
  if (cmdH("Gr")) {
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToHms(reply, radToHrs(gotoTarget.r), false, precisionMode);
    *numericReply = false;
  } else

  // :GT#         Get tracking rate, 0.0 unless TrackingSidereal
  //              Returns: n.n# (OnStep returns more decimal places than LX200 standard)
  if (cmd("GT"))  {
    if (trackingState == TS_NONE) strcpy(reply,"0"); else dtostrf(siderealToHz(trackingRate), 0, 5, reply);
    *numericReply = false;
  } else 

  // :GU#       Get telescope Status
  //            Returns: s#
  if (cmd("GU"))  {
    int i = 0;
    if (trackingState == TS_NONE)            reply[i++]='n';                                             // [n]ot tracking
    if (gotoState == GS_NONE)                reply[i++]='N';                                             // [N]o goto
    if (parkState == PS_UNPARKED)            reply[i++]='p'; else
    if (parkState == PS_PARKING)             reply[i++]='I'; else
    if (parkState == PS_PARKED)              reply[i++]='P'; else
    if (parkState == PS_PARK_FAILED)         reply[i++]='F';                                             // not [p]arked, parking [I]n-progress, [P]arked, Park [F]ailed
    if (pecRecorded)                         reply[i++]='R';                                             // PEC data has been [R]ecorded
  //if (syncToEncodersOnly)                  reply[i++]='e';                                             // sync to [e]ncoders only
    if (atHome)                              reply[i++]='H';                                             // at [H]ome
  //if (ppsSynced)                           reply[i++]='S';                                             // PPS [S]ync
    if (guideState != GU_NONE)               reply[i++]='g';                                             // [g]uide active
    if (guideState != GU_PULSE_GUIDE)        reply[i++]='G';                                             // pulse [G]uide active
    if (mountType != ALTAZM) {
      if (rateCompensation == RC_REFR_RA)  { reply[i++]='r'; reply[i++]='s'; }                           // [r]efr enabled [s]ingle axis
      if (rateCompensation == RC_REFR_BOTH){ reply[i++]='r'; }                                           // [r]efr enabled
      if (rateCompensation == RC_FULL_RA)  { reply[i++]='t'; reply[i++]='s'; }                           // on[t]rack enabled [s]ingle axis
      if (rateCompensation == RC_FULL_BOTH){ reply[i++]='t'; }                                           // on[t]rack enabled
    }
    if (waitingHome)                         reply[i++]='w';                                             // [w]aiting at home 
    if (pauseHome)                           reply[i++]='u';                                             // pa[u]se at home enabled?
    if (soundEnabled)                        reply[i++]='z';                                             // bu[z]zer enabled?
    if (mountType==GEM && autoMeridianFlip)  reply[i++]='a';                                             // [a]uto meridian flip
    #if AXIS1_PEC == ON
      if (mountType != ALTAZM) {
        const char *pch = PECStatusStringAlt;  reply[i++]=pch[pecStatus];                                  // PEC Status one of "/,~;^" (/)gnore, ready to (,)lay, (~)laying, ready to (;)ecord, (^)ecording
      }
    #endif
    // provide mount type
    if (mountType == GEM)                    reply[i++]='E'; else
    if (mountType == FORK)                   reply[i++]='K'; else
    if (mountType == ALTAZM)                 reply[i++]='A';

    // provide pier side info.
    if (current.pierSide == PIER_SIDE_NONE)  reply[i++]='o'; else                                      // pier side n[o]ne
    if (current.pierSide == PIER_SIDE_EAST)  reply[i++]='T'; else                                      // pier side eas[T]
    if (current.pierSide == PIER_SIDE_WEST)  reply[i++]='W';                                           // pier side [W]est

    // provide pulse-guide rate
    reply[i++]='0'+pulseGuideRate;

    // provide guide rate
    reply[i++]='0'+guideRate;

    // provide general error
    reply[i++]='0'; //+generalError;
    reply[i++]=0;

    *numericReply = false;
  } else

  // :Gu#       Get bit packed telescope status
  //            Returns: s#
  if (cmd("Gu")) {
    memset(reply, (char)0b10000000, 9);
    if (trackingState == TS_NONE)                reply[0]|=0b10000001;           // Not tracking
    if (gotoState == GS_NONE)                    reply[0]|=0b10000010;           // No goto
//  if (ppsSynced)                               reply[0]|=0b10000100;           // PPS sync
    if (guideState == GU_PULSE_GUIDE)            reply[0]|=0b10001000;           // pulse guide active
    if (mountType != ALTAZM) {
      if (rateCompensation == RC_REFR_RA)        reply[0]|=0b11010000;           // Refr enabled Single axis
      if (rateCompensation == RC_REFR_BOTH)      reply[0]|=0b10010000;           // Refr enabled
      if (rateCompensation == RC_FULL_RA)        reply[0]|=0b11100000;           // OnTrack enabled Single axis
      if (rateCompensation == RC_FULL_BOTH)      reply[0]|=0b10100000;           // OnTrack enabled
    }
    if (rateCompensation == RC_NONE) {
      double r = siderealToHz(trackingRate);
      if (fequal(r, 57.900))                     reply[1]|=0b10000001; else      // Lunar rate selected
      if (fequal(r, 60.000))                     reply[1]|=0b10000010; else      // Solar rate selected
      if (fequal(r, 60.136))                     reply[1]|=0b10000011;           // King rate selected
    }
    
    if (syncToEncodersOnly)                      reply[1]|=0b10000100;           // sync to encoders only
    if (guideState != GU_NONE)                   reply[1]|=0b10001000;           // guide active
    if (atHome)                                  reply[2]|=0b10000001;           // At home
    if (waitingHome)                             reply[2]|=0b10000010;           // Waiting at home
    if (pauseHome)                               reply[2]|=0b10000100;           // Pause at home enabled?
    if (soundEnabled)                            reply[2]|=0b10001000;           // Buzzer enabled?
    if (mountType == GEM && autoMeridianFlip)    reply[2]|=0b10010000;           // Auto meridian flip
    if (pecRecorded)                             reply[2]|=0b10100000;           // PEC data has been recorded

    // provide mount type
    if (mountType == GEM)                        reply[3]|=0b10000001; else      // GEM
    if (mountType == FORK)                       reply[3]|=0b10000010; else      // FORK
    if (mountType == ALTAZM)                     reply[3]|=0b10001000;           // ALTAZM

    // provide pier side info.
    updatePosition();
    if (current.pierSide == PIER_SIDE_NONE)      reply[3]|=0b10010000; else      // Pier side none
    if (current.pierSide == PIER_SIDE_EAST)      reply[3]|=0b10100000; else      // Pier side east
    if (current.pierSide == PIER_SIDE_WEST)      reply[3]|=0b11000000;           // Pier side west

#if AXIS1_PEC == ON
    if (mountType != ALTAZM) {
      reply[4] = (int)pecState|0b10000000;                                       // PEC status: 0 ignore, 1 ready play, 2 playing, 3 ready record, 4 recording
    }
#endif
    reply[5] = (int)parkState|0b10000000;                                        // Park status: 0 not parked, 1 parking in-progress, 2 parked, 3 park failed
    reply[6] = (int)pulseGuideRate|0b10000000;                                   // Pulse-guide rate
    reply[7] = (int)guideRate|0b10000000;                                        // Guide rate
//  reply[8] = generalError|0b10000000;                                          // General error
    reply[9] = 0;
    *numericReply=false;
  } else

  if (cmdP("GX") && parameter[0] == '9') { // 9n: Misc.
    *numericReply = false;
    switch (parameter[1]) {
//    case '0': dtostrf(guideRates[getPulseGuideRateSelection()]/15.0,2,2,reply); break; // pulse-guide rate
//    case '1': sprintf(reply,"%d",pecValue); break;                             // pec analog value
//    case '2': dtostrf(maxRateCurrent,3,3,reply); break;                        // MaxRate (current)
//    case '3': dtostrf(maxRateDefault,3,3,reply); break;                        // MaxRate (default)
      case '4': sprintf(reply,"%d%s",(int)current.pierSide,(meridianFlip == MF_NEVER)?" N":""); break; // pierSide (N if never)
      case '5': sprintf(reply,"%d",(int)limits.autoMeridianFlip); break;         // autoMeridianFlip
      case '6': reply[0] = "EWB"[preferredPierSide-10]; reply[1] = 0; break;     // preferred pier side
      case '7': dtostrf((1000000.0/maxRateCurrent)/degToRad(axis1.getStepsPerMeasure()),3,1,reply); break; // slew speed
      case '8':                                                                  // rotator availablity 2=rotate/derotate, 1=rotate, 0=off
        if (ROTATOR == ON) {
          if (mountType == ALTAZM) strcpy(reply,"D"); else strcpy(reply,"R");
        } else strcpy(reply,"N");
      break;
//    case '9': dtostrf(maxRateLowerLimit()/16.0,3,3,reply); break;              // MaxRate (fastest/lowest)
//    case 'A': dtostrf(ambient.getTemperature(),3,1,reply); break;              // temperature in deg. C
//    case 'B': dtostrf(ambient.getPressure(),3,1,reply); break;                 // pressure in mb
//    case 'C': dtostrf(ambient.getHumidity(),3,1,reply); break;                 // relative humidity in %
//    case 'D': dtostrf(ambient.getAltitude(),3,1,reply); break;                 // altitude in meters
//    case 'E': dtostrf(ambient.getDewPoint(),3,1,reply); break;                 // dew point in deg. C
//    case 'F':                                                                  // internal MCU temperature in deg. C
//      float t=HAL_MCU_Temperature();
//      if (t > -999) dtostrf(t,1,0,reply); else { *numericReply = true; commandError=CE_0; }
//    break;
    default:
      *numericReply = true;
      *commandError = CE_CMD_UNKNOWN;
    }
  } else

  if (cmdP("GX") && parameter[0] == 'E') { // En: Get settings
    *numericReply = false;
    switch (parameter[1]) {
//    case '1': dtostrf((double)maxRateBaseActual,3,3,reply); break;
//    case '2': dtostrf(SLEW_ACCELERATION_DIST,2,1,reply); break;
//    case '3': sprintf(reply,"%ld",lround(TRACK_BACKLASH_RATE)); break;
      case '4': sprintf(reply,"%ld",lround(axis1Settings.stepsPerMeasure/RAD)); break;
      case '5': sprintf(reply,"%ld",lround(axis2Settings.stepsPerMeasure/RAD)); break;
//    case '6': dtostrf(stepsPerSecondAxis1,3,6,reply); break;
//    case '7': sprintf(reply,"%ld",nv.readLong(EE_stepsPerWormRotAxis1)); break;
//    case '8': sprintf(reply,"%ld",lround(pecBufferSize)); break;
      case '9': sprintf(reply,"%ld",lround(radToDeg(limits.pastMeridianE)*4)); break; // minutes past meridianE
      case 'A': sprintf(reply,"%ld",lround(radToDeg(limits.pastMeridianW)*4)); break; // minutes past meridianW
      case 'e': sprintf(reply,"%ld",lround(radToDeg(axis1Settings.min))); break;      // RA east or -Az limit, in degrees
      case 'w': sprintf(reply,"%ld",lround(radToDeg(axis1Settings.max))); break;      // RA west or +Az limit, in degrees
      case 'B': sprintf(reply,"%ld",lround(radToDeg(axis1Settings.max)/15.0)); break; // RA west or +Az limit, in hours
      case 'C': sprintf(reply,"%ld",lround(radToDeg(axis2Settings.min))); break;
      case 'D': sprintf(reply,"%ld",lround(radToDeg(axis2Settings.max))); break;
      case 'E': reply[0] = '0' + (MOUNT_COORDS - 1); *supressFrame = true; break;
      case 'F': if (AXIS2_TANGENT_ARM == ON) reply[0] = '1'; else reply[0] = '0'; *supressFrame = true; break;
//    case 'M': if (!runtimeSettings()) strcpy(reply, "0"); else sprintf(reply,"%d",(int)nv.read(EE_mountType)); break; // return the mount type
    default:
      *numericReply = true;
      *commandError = CE_CMD_UNKNOWN;
    }
  } else

  // :GZ#       Get Mount Azimuth
  //            Returns: DDD*MM# or DDD*MM'SS# (based on precision setting)
  // :GZH#      High precision
  //            Returns: DDD*MM'SS.SSS# (high precision)
  if (cmdH("GZ")) {
    updatePosition();
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToDms(reply, radToDeg(transform.mountToNative(&current, true).z), true, false, precisionMode);
    *numericReply = false;
  } else

  // :Gz#       Get Target Azimuth
  //            Returns: DDD*MM# or DDD*MM'SS# (based on precision setting)
  // :GzH#      High precision
  //            Returns: DDD*MM'SS.SSS# (high precision)
  if (cmdH("Gz")) {
    if (parameter[0] == 'H') precisionMode = PM_HIGHEST;
    convert.doubleToDms(reply, radToDeg(gotoTarget.z), false, true, precisionMode);
    *numericReply = false;
  } else

  //  :Sa[sDD*MM]# or :Sa[sDD*MM'SS]# or :Sa[sDD*MM'SS.SSS]#
  //            Set Target Altitude
  //            Return: 0 on failure
  //                    1 on success
  if (cmdP("Sa")) {
    if (!convert.dmsToDouble(&gotoTarget.a, parameter, true)) *commandError = CE_PARAM_RANGE;
    gotoTarget.a = degToRad(gotoTarget.a);
  } else

  //  :Sd[sDD*MM]# or :Sd[sDD*MM:SS]# or :Sd[sDD*MM:SS.SSS]#
  //            Set Target Declination
  //            Return: 0 on failure
  //                    1 on success
  if (cmdP("Sd")) {
    if (!convert.dmsToDouble(&gotoTarget.d, parameter, true)) *commandError = CE_PARAM_RANGE;
    gotoTarget.d = degToRad(gotoTarget.d);
  } else

  //  :Sh[sDD]#
  //            Set the elevation lower limit
  //            Return: 0 on failure
  //                    1 on success
  if (cmdP("Sh")) {
    int16_t deg;
    if (convert.atoi2(parameter, &deg)) {
      if (deg >= -30 && deg <= 30) {
        limits.minAltitude = degToRad(deg);
//      nv.update(EE_minAlt, minAlt+128);
      } else *commandError = CE_PARAM_RANGE;
    } else *commandError = CE_PARAM_FORM;
  } else

  //  :So[DD]#
  //            Set the overhead elevation limit in degrees relative to the horizon
  //            Return: 0 on failure
  //                    1 on success
  if (command[1] == 'o')  {
    int16_t deg;
    if (convert.atoi2(parameter, &deg)) {
      if (deg >= 60 && deg <= 90) {
        limits.maxAltitude = degToRad(deg);
        if (mountType == ALTAZM && limits.maxAltitude > 87) limits.maxAltitude = 87;
//      nv.update(EE_maxAlt,maxAlt); 
      } else *commandError = CE_PARAM_RANGE;
    } else *commandError = CE_PARAM_FORM;
  } else

  //  :Sr[HH:MM.T]# or :Sr[HH:MM:SS]# or :Sr[HH:MM:SS.SSSS]#
  //            Set Target Right Ascension
  //            Return: 0 on failure
  //                    1 on success
  if (cmdP("Sr")) {
    if (!convert.hmsToDouble(&gotoTarget.r, parameter)) *commandError = CE_PARAM_RANGE;
    gotoTarget.r = hrsToRad(gotoTarget.r);
  } else

  //  :ST[H.H]# Set Tracking Rate in Hz where 60.0 is solar rate
  //            Return: 0 on failure
  //                    1 on success
  if (cmdP("ST"))  {
    double f = strtod(parameter,&conv_end);
    if (&parameter[0] != conv_end && ((f >= 30.0 && f < 90.0) || fabs(f) < 0.1)) {
      if (fabs(f) < 0.1) trackingState = TS_NONE; else {
        if (trackingState == TS_NONE) { trackingState = TS_SIDEREAL; axis1.enable(true); axis2.enable(true); }
        trackingRate = hzToSidereal(f);
      }
      updateTrackingRates();
    } else *commandError = CE_PARAM_RANGE;
  } else

  //  :Sz[DDD*MM]# or :Sz[DDD*MM'SS]# or :Sz[DDD*MM'SS.SSS]#
  //            Set Target Azmuith
  //            Return: 0 on failure
  //                    1 on success
  if (cmdP("Sz")) {
    if (!convert.dmsToDouble(&gotoTarget.z, parameter, false)) *commandError = CE_PARAM_RANGE;
    gotoTarget.z = degToRad(gotoTarget.z);
  } else

  // T - Tracking Commands
  //
  // :To#       Track full compensation on
  // :Tr#       Track refraction compensation on
  // :Tn#       Track compensation off
  // :T1#       Track dual axis off (disable Dec tracking on Eq mounts)
  // :T2#       Track dual axis on
  //
  // :TS#       Track rate solar
  // :TK#       Track rate king
  // :TL#       Track rate lunar
  // :TQ#       Track rate sidereal
  //
  // :T+#       Master sidereal clock faster by 0.02 Hertz (stored in EEPROM)
  // :T-#       Master sidereal clock slower by 0.02 Hertz (stored in EEPROM)
  // :TR#       Master sidereal clock reset (to calculated sidereal rate, stored in EEPROM)
  //            Returns: Nothing
  //
  // :Te#       Tracking enable
  // :Td#       Tracking disable
  //            Return: 0 on failure
  //                    1 on success
  if (command[0] == 'T' && parameter[0] == 0) {
    if (command[1] == 'o' && mountType != ALTAZM) { rateCompensation = RC_FULL_RA; } else            // full compensation on
    if (command[1] == 'r' && mountType != ALTAZM) { rateCompensation = RC_REFR_RA; } else            // refraction compensation on
    if (command[1] == 'n' && mountType != ALTAZM) { rateCompensation = RC_NONE;    }                 // compensation off
    if (command[1] == '1' && mountType != ALTAZM) {                                                  // dual axis tracking off
      if (rateCompensation == RC_REFR_BOTH) rateCompensation = RC_REFR_RA; else if (rateCompensation == RC_FULL_BOTH) rateCompensation = RC_FULL_RA;
    } else
    if (command[1] == '2' && mountType != ALTAZM) {                                                  // dual axis tracking on
      if (rateCompensation == RC_REFR_RA) rateCompensation = RC_REFR_BOTH; else if (rateCompensation == RC_FULL_RA) rateCompensation = RC_FULL_BOTH;
    } else
    if (command[1] == 'S') { rateCompensation = RC_NONE; trackingRate = hzToSidereal(60);     } else // solar tracking rate 60Hz
    if (command[1] == 'K') { rateCompensation = RC_NONE; trackingRate = hzToSidereal(60.136); } else // king  tracking rate 60.136Hz
    if (command[1] == 'L') { rateCompensation = RC_NONE; trackingRate = hzToSidereal(57.9);   } else // lunar tracking rate 57.9Hz
    if (command[1] == 'Q') { trackingRate = hzToSidereal(SIDEREAL_RATE_HZ);                   } else // sidereal tracking rate 60.164Hz
    if (command[1] == '+') { clock.setPeriodSubMicros(clock.getPeriodSubMicros() - hzToSubMicros(0.02)); } else
    if (command[1] == '-') { clock.setPeriodSubMicros(clock.getPeriodSubMicros() + hzToSubMicros(0.02)); } else
    if (command[1] == 'R') { clock.setPeriodSubMicros(SIDEREAL_PERIOD); } else                       // reset master sidereal clock interval
    if (command[1] == 'e') {
      if (parkState != PS_PARKED) { resetGeneralErrors(); trackingState = TS_SIDEREAL; axis1.enable(true); axis2.enable(true); } else *commandError = CE_PARKED;
    } else
    if (command[1] == 'd') {
      if (gotoState == GS_NONE && guideState == GU_NONE) trackingState = TS_NONE; else *commandError = CE_MOUNT_IN_MOTION;
    } else *commandError = CE_CMD_UNKNOWN;

    if (*commandError == CE_NONE) {
      switch (command[1]) { case 'S': case 'K': case 'L': case 'Q': case '+': case '-': case 'R': *numericReply = false; }
      switch (command[1]) { case 'o': case 'r': case 'n': trackingRate = hzToSidereal(SIDEREAL_RATE_HZ); }
      updateTrackingRates();
    }
  } else

  // :VS#       PEC number of steps per second of worm rotation
  //            Returns: n.n#
  if (cmd("VS")) {
    char temp[12];
    dtostrf(stepsPerSecondAxis1, 0, 6, temp);
    strcpy(reply, temp);
    *numericReply = false;
  } else

  //  $ - Set parameter
  // :$BD[n]#   Set Dec/Alt backlash in arc-seconds
  //            Return: 0 on failure
  //                    1 on success
  // :$BR[n]#   Set RA/Azm backlash in arc-seconds
  //            Return: 0 on failure
  //                    1 on success
  //        Set the Backlash values.  Units are arc-seconds
  if (cmdP("$B")) {
    int16_t arcSecs;
    if (convert.atoi2((char*)&parameter[1], &arcSecs)) {
      if (arcSecs >= 0 && arcSecs <= 3600) {
        if (parameter[0] == 'D') {
          axis2.setBacklash(arcsecToRad(arcSecs));
//        nv.writeInt(EE_backlashAxis2,backlashAxis2);
        } else
        if (parameter[0] == 'R') {
          axis1.setBacklash(arcsecToRad(arcSecs));
//        nv.writeInt(EE_backlashAxis1,backlashAxis1);
        } else *commandError = CE_CMD_UNKNOWN;
      } else *commandError = CE_PARAM_RANGE;
    } else *commandError = CE_PARAM_FORM;
  } else

  //  % - Return parameter
  // :%BD#      Get Dec/Alt Antibacklash value in arc-seconds
  //            Return: n#
  // :%BR#      Get RA/Azm Antibacklash value in arc-seconds
  //            Return: n#
  if (cmdP("%B")) {
    if (parameter[0] == 'D' && parameter[1] == 0) {
        int arcSec = radToArcsec(axis2.getBacklash());
        if (arcSec < 0) arcSec = 0; if (arcSec > 3600) arcSec = 3600;
        sprintf(reply,"%d", arcSec);
        *numericReply = false;
    } else
    if (parameter[0] == 'R' && parameter[1] == 0) {
        int arcSec = radToArcsec(axis2.getBacklash());
        if (arcSec < 0) arcSec = 0; if (arcSec > 3600) arcSec = 3600;
        sprintf(reply,"%d", arcSec);
        *numericReply = false;
    } else *commandError = CE_CMD_UNKNOWN;
  } else return false;

  return true;
}

void Mount::updatePosition() {
  current = transform.instrumentToMount(axis1.getInstrumentCoordinate(), axis2.getInstrumentCoordinate());
}

void Mount::updateTrackingRates() {
  if (mountType != ALTAZM) {
    trackingRateAxis1 = trackingRate;
    if (rateCompensation != RC_REFR_BOTH && rateCompensation != RC_FULL_BOTH) trackingRateAxis2 = 0;
  }
  if (trackingState == TS_NONE) { trackingRateAxis1 = 0; trackingRateAxis2 = 0; }
  axis1.setFrequency(siderealToRad(trackingRateAxis1 + guideRateAxis1 + deltaRateAxis1));
  axis2.setFrequency(siderealToRad(trackingRateAxis2 + guideRateAxis2 + deltaRateAxis2));
}

void Mount::resetGeneralErrors() {
  generalErrors.altitudeMin = false;
  generalErrors.limitSense = false;
  generalErrors.decMinMax = false;
  generalErrors.azmMinMax = false;
  generalErrors.raMinMax  = false;
  generalErrors.raMeridian = false;
  generalErrors.sync = false;
  generalErrors.altitudeMax = false;
  generalErrors.park = false;
}

#endif
