////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (c) 2016-2017 Leonardo Consoni <consoni_2519@hotmail.com>       //
//                                                                            //
//  This file is part of Signal-IO-NIXNET.                                    //
//                                                                            //
//  Signal-IO-NIXNETs free software: you can redistribute it and/or modify    //
//  it under the terms of the GNU Lesser General Public License as published  //
//  by the Free Software Foundation, either version 3 of the License, or      //
//  (at your option) any later version.                                       //
//                                                                            //
//  Signal-IO-NIXNET is distributed in the hope that it will be useful,       //
//  but WITHOUT ANY WARRANTY; without even the implied warranty of            //
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the              //
//  GNU Lesser General Public License for more details.                       //
//                                                                            //
//  You should have received a copy of the GNU Lesser General Public License  //
//  along with Signal-IO-NIXNET. If not, see <http://www.gnu.org/licenses/>.  //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////


#include "signal_io/signal_io.h"
#include "can_network.h"

#include "debug/data_logging.h"
#include "timing/timing.h"

#include "khash.h"

enum { INPUT_POSITION, INPUT_VELOCITY, INPUT_CURRENT, INPUT_ANALOG, INPUT_CHANNELS_NUMBER };
enum { OUTPUT_POSITION, OUTPUT_VELOCITY, OUTPUT_CURRENT, OUTPUT_CHANNELS_NUMBER };

enum States { READY_2_SWITCH_ON = 1, SWITCHED_ON = 2, OPERATION_ENABLED = 4, FAULT = 8, VOLTAGE_ENABLED = 16, 
              QUICK_STOPPED = 32, SWITCH_ON_DISABLE = 64, REMOTE_NMT = 512, TARGET_REACHED = 1024, SETPOINT_ACK = 4096 };

enum Controls { SWITCH_ON = 1, ENABLE_VOLTAGE = 2, QUICK_STOP = 4, ENABLE_OPERATION = 8, 
                NEW_SETPOINT = 16, CHANGE_IMMEDIATEDLY = 32, ABS_REL = 64, FAULT_RESET = 128, HALT = 256 };

typedef struct _SignalIOTaskData
{
  CANFrame readFramesList[ CAN_FRAME_TYPES_NUMBER ];
  CANFrame writeFramesList[ CAN_FRAME_TYPES_NUMBER ];
  uint16_t statusWord, controlWord;
  double measuresList[ INPUT_CHANNELS_NUMBER ];
  bool isReading, isOutputChannelUsed; 
  uint8_t readPayload[ 8 ], writePayload[ 8 ];
}
SignalIOTaskData;

typedef SignalIOTaskData* SignalIOTask;

KHASH_MAP_INIT_INT( TaskInt, SignalIOTask )
static khash_t( TaskInt )* tasksList = NULL;

DECLARE_MODULE_INTERFACE( SIGNAL_IO_INTERFACE ); 

static SignalIOTask LoadTaskData( const char* );
static void UnloadTaskData( SignalIOTask );

static void* AsyncReadBuffer( void* );
static void EnableOutput( SignalIOTask, bool );

int InitDevice( const char* taskConfig )
{
  if( tasksList == NULL ) tasksList = kh_init( TaskInt );
  
  int taskKey = (int) kh_str_hash_func( taskConfig );
  
  //DEBUG_PRINT( "trying to create task on node %s", taskConfig ); 
  
  int insertionStatus;
  khint_t newTaskIndex = kh_put( TaskInt, tasksList, taskKey, &insertionStatus );
  if( insertionStatus > 0 )
  {
    kh_value( tasksList, newTaskIndex ) = LoadTaskData( taskConfig );
    if( kh_value( tasksList, newTaskIndex ) == NULL )
    {
      DEBUG_PRINT( "loading task %s failed", taskConfig );
      EndDevice( taskKey ); 
      return -1;
    }
        
    //DEBUG_PRINT( "task %s key %d inserted (iterator: %u - total: %u)", taskConfig, kh_key( tasksList, newTaskIndex ), newTaskIndex, kh_size( tasksList ) );
  }
  //else if( insertionStatus == 0 ) { DEBUG_PRINT( "task %s (key %d) already exists (iterator %u)", taskConfig, taskKey, newTaskIndex ); }
  
  return (int) kh_key( tasksList, newTaskIndex );
}

void EndDevice( int taskID )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  task->isReading = false;
  
  EnableOutput( task, false );
  
  UnloadTaskData( task );
  
  kh_del( TaskInt, tasksList, (khint_t) taskID );
  
  if( kh_size( tasksList ) == 0 )
  {
    kh_destroy( TaskInt, tasksList );
    tasksList = NULL;
  }
}

size_t GetMaxInputSamplesNumber( int taskID )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return 0;
  
  return 1;
}

size_t Read( int taskID, unsigned int channel, double* ref_value )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return 0;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( channel >= INPUT_CHANNELS_NUMBER ) return 0;
  
  CANNetwork_Sync();
  
  // Read values from PDO01 (Position, Current and Status Word) to buffer
  CANFrame_Read( task->readFramesList[ PDO01 ], task->readPayload );  
  // Update values from PDO01
  task->measuresList[ INPUT_POSITION ] = task->readPayload[ 3 ] * 0x1000000 + task->readPayload[ 2 ] * 0x10000 + task->readPayload[ 1 ] * 0x100 + task->readPayload[ 0 ];
  int currentHEX = task->readPayload[ 5 ] * 0x100 + task->readPayload[ 4 ];
  double currentMA = currentHEX - ( ( currentHEX >= 0x8000 ) ? 0xFFFF : 0 );
  task->measuresList[ INPUT_CURRENT ] = currentMA / 1000.0;
  
  //DEBUG_PRINT( "current: hex=%d, mA=%d, A=%g", currentHEX, currentHEX - ( ( currentHEX >= 0x8000 ) ? 0xFFFF : 0 ), task->measuresList[ INPUT_CURRENT ] );
  
  task->statusWord = task->readPayload[ 7 ] * 0x100 + task->readPayload[ 6 ];
  
  // Read values from PDO02 (Velocity and Tension) to buffer
  CANFrame_Read( task->readFramesList[ PDO02 ], task->readPayload );  
  // Update values from PDO02
  task->measuresList[ INPUT_VELOCITY ] = task->readPayload[ 3 ] * 0x1000000 + task->readPayload[ 2 ] * 0x10000 + task->readPayload[ 1 ] * 0x100 + task->readPayload[ 0 ];
  task->measuresList[ INPUT_ANALOG ] = task->readPayload[ 5 ] * 0x100 + task->readPayload[ 4 ];
    
  *ref_value = task->measuresList[ channel ];
  
  return 1;
}

bool HasError( int taskID )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return false;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  //task->statusWord = (uint16_t) CANNetwork_ReadSingleValue( task->writeFramesList[ SDO ], task->readFramesList[ SDO ], 0x6041, 0x00 );

  return (bool) ( task->statusWord & FAULT );
}

void Reset( int taskID )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  task->controlWord |= FAULT_RESET;
  CANNetwork_WriteSingleValue( task->writeFramesList[ SDO ], 0x6040, 0x00, task->controlWord );
  
  Time_Delay( 200 );
  
  task->controlWord &= (~FAULT_RESET);
  CANNetwork_WriteSingleValue( task->writeFramesList[ SDO ], 0x6040, 0x00, task->controlWord );
}

bool CheckInputChannel( int taskID, unsigned int channel )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return false;
  
  if( channel >= INPUT_CHANNELS_NUMBER ) return false;
  
  return true;
}

bool Write( int taskID, unsigned int channel, double value )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return false;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  int encoderSetpoint = (int) value;
  
  double currentSetpointMA = value * 1000.0;
  int16_t currentSetpointHEX = (int16_t) currentSetpointMA + ( ( currentSetpointMA < 0.0 ) ? 0xFFFF : 0 );
  //DEBUG_PRINT( "current setpoint: %d (hex: %d)", (int16_t) currentSetpointMA, currentSetpointHEX );
  
  // Set values for PDO01 (Position Setpoint and Control Word)
  task->writePayload[ 0 ] = (uint8_t) ( encoderSetpoint & 0x000000FF );
  task->writePayload[ 1 ] = (uint8_t) ( ( encoderSetpoint & 0x0000FF00 ) / 0x100 );
  task->writePayload[ 2 ] = (uint8_t) ( ( encoderSetpoint & 0x00FF0000 ) / 0x10000 );
  task->writePayload[ 3 ] = (uint8_t) ( ( encoderSetpoint & 0xFF000000 ) / 0x1000000 );
  task->writePayload[ 4 ] = (uint8_t) ( currentSetpointHEX & 0x000000FF );
  task->writePayload[ 5 ] = (uint8_t) ( ( currentSetpointHEX & 0x0000FF00 ) / 0x100 ); 
  task->writePayload[ 6 ] = (uint8_t) ( task->controlWord & 0x000000FF );
  task->writePayload[ 7 ] = (uint8_t) ( ( task->controlWord & 0x0000FF00 ) / 0x100 ); 
  
  // Write values from buffer to PDO01 
  CANFrame_Write( task->writeFramesList[ PDO01 ], task->writePayload );
  
  int velocitySetpointRPM = (int) value;
  int16_t digitalOutput = (int16_t) value;
  
  // Set values for PDO02 (Velocity Setpoint and Digital Output)
  task->writePayload[ 0 ] = (uint8_t) ( velocitySetpointRPM & 0x000000FF );
  task->writePayload[ 1 ] = (uint8_t) ( ( velocitySetpointRPM & 0x0000FF00 ) / 0x100 );
  task->writePayload[ 2 ] = (uint8_t) ( ( velocitySetpointRPM & 0x00FF0000 ) / 0x10000 );
  task->writePayload[ 3 ] = (uint8_t) ( ( velocitySetpointRPM & 0xFF000000 ) / 0x1000000 );
  task->writePayload[ 4 ] = (uint8_t) ( digitalOutput & 0x000000FF );
  task->writePayload[ 5 ] = (uint8_t) ( ( digitalOutput & 0x0000FF00 ) / 0x100 ); 
  task->writePayload[ 6 ] = ( 0 & 0x000000FF );
  task->writePayload[ 7 ] = ( 0 & 0x0000FF00 ) / 0x100; 
  
  // Write values from buffer to PDO01
  CANFrame_Write( task->writeFramesList[ PDO02 ], task->writePayload );
  
  CANNetwork_Sync();
  
  return true;
}

void EnableOutput( SignalIOTask task, bool enable )
{
  task->controlWord |= SWITCH_ON;
  task->controlWord &= (~ENABLE_OPERATION);
  CANNetwork_WriteSingleValue( task->writeFramesList[ SDO ], 0x6040, 0x00, task->controlWord );
  
  Time_Delay( 200 );
  
  if( enable ) task->controlWord |= ENABLE_OPERATION;
  else task->controlWord &= (~SWITCH_ON);
    
  CANNetwork_WriteSingleValue( task->writeFramesList[ SDO ], 0x6040, 0x00, task->controlWord );
}

//bool IsOutputEnabled( int taskID )
//{
//  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
//  if( taskIndex == kh_end( tasksList ) ) return false;
  
//  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  //task->statusWord = (uint16_t) CANNetwork_ReadSingleValue( task->writeFramesList[ SDO ], task->readFramesList[ SDO ], 0x6041, 0x00 );
  
//  return (bool) ( task->statusWord & ( SWITCHED_ON | OPERATION_ENABLED ) );
//}

bool AcquireOutputChannel( int taskID, unsigned int channel )
{
  const int OPERATION_MODES[ OUTPUT_CHANNELS_NUMBER ] = { 0xFF, 0xFE, 0xFD };
  
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return false;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( channel >= OUTPUT_CHANNELS_NUMBER ) return false;
  
  if( task->isOutputChannelUsed ) return false;
  
  DEBUG_PRINT( "setting operation mode %X", OPERATION_MODES[ channel ] );
  
  CANNetwork_WriteSingleValue( task->writeFramesList[ SDO ], 0x6060, 0x00, OPERATION_MODES[ channel ] );
  
  EnableOutput( task, true );
  
  task->isOutputChannelUsed = true;
  
  return true;
}

void ReleaseOutputChannel( int taskID, unsigned int channel )
{
  khint_t taskIndex = kh_get( TaskInt, tasksList, (khint_t) taskID );
  if( taskIndex == kh_end( tasksList ) ) return;
  
  SignalIOTask task = kh_value( tasksList, taskIndex );
  
  if( channel >= OUTPUT_CHANNELS_NUMBER ) return;
  
  CANNetwork_WriteSingleValue( task->writeFramesList[ SDO ], 0x6060, 0x00, 0x00 );
  
  EnableOutput( task, false );
  
  task->isOutputChannelUsed = false;
}

SignalIOTask LoadTaskData( const char* taskConfig )
{
  bool loadError = false;
  
  SignalIOTask newTask = (SignalIOTask) malloc( sizeof(SignalIOTaskData) );
  memset( newTask, 0, sizeof(SignalIOTaskData) );
  
  unsigned int nodeID = (unsigned int) strtoul( taskConfig, NULL, 0 );
  
  //DEBUG_PRINT( "trying to load CAN interface for node %u", nodeID );
  
  for( size_t frameType = 0; frameType < CAN_FRAME_TYPES_NUMBER; frameType++ )
  {
    if( (newTask->readFramesList[ frameType ] = CANNetwork_InitFrame( frameType, FRAME_IN, nodeID )) == NULL ) loadError = true; 
    if( (newTask->writeFramesList[ frameType ] = CANNetwork_InitFrame( frameType, FRAME_OUT, nodeID )) == NULL ) loadError = true;
  }
  
  newTask->isOutputChannelUsed = false;
  
  if( loadError )
  {
    UnloadTaskData( newTask );
    return NULL;
  }
  
  newTask->controlWord = ENABLE_VOLTAGE | QUICK_STOP;
  CANNetwork_WriteSingleValue( newTask->writeFramesList[ SDO ], 0x6040, 0x00, newTask->controlWord );
  
  return newTask;
}

void UnloadTaskData( SignalIOTask task )
{
  if( task == NULL ) return;
  
  DEBUG_PRINT( "ending task %p", task );
  
  for( size_t frameID = 0; frameID < CAN_FRAME_TYPES_NUMBER; frameID++ )
  {
    CANNetwork_EndFrame( task->readFramesList[ frameID ] ); 
    CANNetwork_EndFrame( task->writeFramesList[ frameID ] );
  }
  
  free( task );
}
