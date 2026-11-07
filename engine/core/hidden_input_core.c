#include "hidden_input_core.h"

void hidden_input_core_init(
      hidden_input_state_t *state )
{
	if( state == NULL )
	{
		return;
	}
	state->length = 0;
}

hidden_input_action_t hidden_input_core_feed(
      hidden_input_state_t *state,
      int c )
{
	if( state == NULL )
	{
		return HIDDEN_INPUT_ACTION_REJECTED;
	}

	if( ( c == '\r' ) || ( c == '\n' ) )
	{
		return HIDDEN_INPUT_ACTION_COMPLETE;
	}
	if( c == 0x03 )
	{
		return HIDDEN_INPUT_ACTION_CANCELLED;
	}
	if( ( c == '\b' ) || ( c == 0x7F ) )
	{
		if( state->length > 0 )
		{
			state->length--;
		}
		return HIDDEN_INPUT_ACTION_CONTINUE;
	}
	if( ( c >= 0x20 ) && ( c <= 0x7E ) )
	{
		if( state->length >= HIDDEN_INPUT_MAX_LENGTH )
		{
			return HIDDEN_INPUT_ACTION_REJECTED;
		}
		state->buffer[ state->length ] = (char) c;
		state->length++;
		return HIDDEN_INPUT_ACTION_CONTINUE;
	}
	return HIDDEN_INPUT_ACTION_REJECTED;
}
