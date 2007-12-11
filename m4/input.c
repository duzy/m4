/* GNU m4 -- A simple macro processor
   Copyright (C) 1989, 1990, 1991, 1992, 1993, 1994, 2006, 2007 Free Software
   Foundation, Inc.

   This file is part of GNU M4.

   GNU M4 is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU M4 is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Handling of different input sources, and lexical analysis.  */

#include <config.h>

#include "m4private.h"

/* Define this to see runtime debug info.  Implied by DEBUG.  */
/*#define DEBUG_INPUT */

/*
   Unread input can be either files that should be read (from the
   command line or by include/sinclude), strings which should be
   rescanned (normal macro expansion text), or quoted builtin
   definitions (as returned by the builtin "defn").  Unread input is
   organized in a stack, implemented with an obstack.  Each input
   source is described by a "struct m4_input_block".  The obstack is
   "input_stack".  The top of the input stack is "isp".

   Each input_block has an associated struct input_funcs, which is a
   vtable that defines polymorphic functions for peeking, reading,
   unget, cleanup, and printing in trace output.  All input is done
   through the function pointers of the input_funcs on the given
   input_block, and all characters are unsigned, to distinguish
   between stdio EOF and between special sentinel characters.  When a
   input_block is exhausted, its reader returns CHAR_RETRY which
   causes the input_block to be popped from the input_stack.

   The macro "m4wrap" places the text to be saved on another input
   stack, on the obstack "wrapup_stack", whose top is "wsp".  When EOF
   is seen on normal input (eg, when "current_input" is empty), input
   is switched over to "wrapup_stack", and the original
   "current_input" is freed.  A new stack is allocated for
   "wrapup_stack", which will accept any text produced by calls to
   "m4wrap" from within the wrapped text.  This process of shuffling
   "wrapup_stack" to "current_input" can continue indefinitely, even
   generating infinite loops (e.g. "define(`f',`m4wrap(`f')')f"),
   without memory leaks.  Adding wrapped data is done through
   m4_push_wrapup().

   Pushing new input on the input stack is done by m4_push_file(), the
   conceptual m4_push_string(), and m4_push_builtin() (for builtin
   definitions).  As an optimization, since most macro expansions
   result in strings, m4_push_string() is split in two parts,
   push_string_init(), which returns a pointer to the obstack for
   growing the output, and push_string_finish(), which returns a
   pointer to the finished input_block.  Thus, instead of creating a
   new input block for every character pushed, macro expansion need
   only add text to the top of the obstack.  However, it is not safe
   to alter the input stack while a string is being constructed.  This
   means the input engine is one of two states: consuming input, or
   collecting a macro's expansion.  The input_block *next is used to
   manage the coordination between the different push routines.

   Normally, input sources behave in LIFO order, resembling a stack.
   But thanks to the defn and m4wrap macros, when collecting the
   expansion of a macro, it is possible that we must intermix multiple
   input blocks in FIFO order.  Therefore, when collecting an
   expansion, a meta-input block is formed which will visit its
   children in FIFO order, without losing data when the obstack is
   cleared in LIFO order.

   The current file and line number are stored in the context, for use
   by the error handling functions in utility.c.  When collecting a
   macro's expansion, these variables can be temporarily inconsistent
   in order to provide better error message locations, but they must
   be restored before further parsing takes place.  Each input block
   maintains its own notion of the current file and line, so swapping
   between input blocks must update the context accordingly.  */

static	int	file_peek		(m4_input_block *);
static	int	file_read		(m4_input_block *, m4 *, bool);
static	void	file_unget		(m4_input_block *, int);
static	void	file_clean		(m4_input_block *, m4 *);
static	void	file_print		(m4_input_block *, m4 *, m4_obstack *);
static	int	builtin_peek		(m4_input_block *);
static	int	builtin_read		(m4_input_block *, m4 *, bool);
static	void	builtin_unget		(m4_input_block *, int);
static	void	builtin_print		(m4_input_block *, m4 *, m4_obstack *);
static	int	string_peek		(m4_input_block *);
static	int	string_read		(m4_input_block *, m4 *, bool);
static	void	string_unget		(m4_input_block *, int);
static	void	string_print		(m4_input_block *, m4 *, m4_obstack *);
static	int	composite_peek		(m4_input_block *);
static	int	composite_read		(m4_input_block *, m4 *, bool);
static	void	composite_unget		(m4_input_block *, int);
static	void	composite_print		(m4_input_block *, m4 *, m4_obstack *);

static	void	make_text_link		(m4_obstack *, m4_symbol_chain **,
					 m4_symbol_chain **);
static	void	init_builtin_token	(m4 *, m4_symbol_value *);
static	bool	match_input		(m4 *, const char *, bool);
static	int	next_char		(m4 *, bool);
static	int	peek_char		(m4 *);
static	bool	pop_input		(m4 *, bool);
static	void	unget_input		(int);
static	bool	consume_syntax		(m4 *, m4_obstack *, unsigned int);

#ifdef DEBUG_INPUT
static int m4_print_token (const char *, m4__token_type, m4_symbol_value *);
#endif

/* Vtable of callbacks for each input method.  */
struct input_funcs
{
  /* Peek at input, return an unsigned char, CHAR_BUILTIN if it is a
     builtin, or CHAR_RETRY if none available.  */
  int	(*peek_func)	(m4_input_block *);

  /* Read input, return an unsigned char, CHAR_BUILTIN if it is a
     builtin, or CHAR_RETRY if none available.  If SAFE, then do not
     alter the current file or line.  */
  int	(*read_func)	(m4_input_block *, m4 *, bool safe);

  /* Unread a single unsigned character or CHAR_BUILTIN, must be the
     same character previously read by read_func.  */
  void	(*unget_func)	(m4_input_block *, int);

  /* Optional function to perform cleanup at end of input.  */
  void	(*clean_func)	(m4_input_block *, m4 *);

  /* Add a representation of the input block to the obstack, for use
     in trace expansion output.  */
  void	(*print_func)	(m4_input_block *, m4 *, m4_obstack *);
};

/* A block of input to be scanned.  */
struct m4_input_block
{
  m4_input_block *prev;		/* Previous input_block on the input stack.  */
  struct input_funcs *funcs;	/* Virtual functions of this input_block.  */
  const char *file;		/* File where this input is from.  */
  int line;			/* Line where this input is from.  */

  union
    {
      struct
	{
	  char *str;		/* String value.  */
	  size_t len;		/* Remaining length.  */
	}
      u_s;	/* See string_funcs.  */
      struct
	{
	  FILE *fp;			/* Input file handle.  */
	  bool_bitfield end : 1;	/* True iff peek returned EOF.  */
	  bool_bitfield close : 1;	/* True to close file on pop.  */
	  bool_bitfield line_start : 1;	/* Saved start_of_input_line state.  */
	}
      u_f;	/* See file_funcs.  */
      struct
	{
	  const m4_builtin *builtin;	/* Pointer to builtin's function.  */
	  m4_module *module;		/* Originating module.  */
	  bool_bitfield read : 1;	/* True iff block has been read.  */
	  int flags : 24;		/* Flags tied to the builtin. */
	  m4_hash *arg_signature;	/* Argument signature for builtin.  */
	}
      u_b;	/* See builtin_funcs.  */
      struct
	{
	  m4_symbol_chain *chain;	/* Current link in chain.  */
	  m4_symbol_chain *end;		/* Last link in chain.  */
	}
      u_c;	/* See composite_funcs.  */
    }
  u;
};


/* Obstack for storing individual tokens.  */
static m4_obstack token_stack;

/* Obstack for storing input file names.  */
static m4_obstack file_names;

/* Wrapup input stack.

   FIXME - m4wrap should be FIFO, which implies a queue, not a stack.
   While fixing this, m4wrap should also remember what the current
   file and line are for each chunk of wrapped text.  */
static m4_obstack *wrapup_stack;

/* Current stack, from input or wrapup.  */
static m4_obstack *current_input;

/* Bottom of token_stack, for obstack_free.  */
static void *token_bottom;

/* Pointer to top of current_input.  */
static m4_input_block *isp;

/* Pointer to top of wrapup_stack.  */
static m4_input_block *wsp;

/* Aux. for handling split m4_push_string ().  */
static m4_input_block *next;

/* Flag for next_char () to increment current_line.  */
static bool start_of_input_line;

/* Flag for next_char () to recognize change in input block.  */
static bool input_change;

/* Vtable for handling input from files.  */
static struct input_funcs file_funcs = {
  file_peek, file_read, file_unget, file_clean, file_print
};

/* Vtable for handling input from builtin functions.  */
static struct input_funcs builtin_funcs = {
  builtin_peek, builtin_read, builtin_unget, NULL, builtin_print
};

/* Vtable for handling input from strings.  */
static struct input_funcs string_funcs = {
  string_peek, string_read, string_unget, NULL, string_print
};

/* Vtable for handling input from composite chains.  */
static struct input_funcs composite_funcs = {
  composite_peek, composite_read, composite_unget, NULL, composite_print
};


/* Input files, from command line or [s]include.  */
static int
file_peek (m4_input_block *me)
{
  int ch;

  ch = me->u.u_f.end ? EOF : getc (me->u.u_f.fp);
  if (ch == EOF)
    {
      me->u.u_f.end = true;
      return CHAR_RETRY;
    }

  ungetc (ch, me->u.u_f.fp);
  return ch;
}

static int
file_read (m4_input_block *me, m4 *context, bool safe M4_GNUC_UNUSED)
{
  int ch;

  if (start_of_input_line)
    {
      start_of_input_line = false;
      m4_set_current_line (context, ++me->line);
    }

  /* If stdin is a terminal, calling getc after peek_char already
     called it would make the user have to hit ^D twice to quit.  */
  ch = me->u.u_f.end ? EOF : getc (me->u.u_f.fp);
  if (ch == EOF)
    {
      me->u.u_f.end = true;
      return CHAR_RETRY;
    }

  if (ch == '\n')
    start_of_input_line = true;
  return ch;
}

static void
file_unget (m4_input_block *me, int ch)
{
  assert (ch < CHAR_EOF);
  if (ungetc (ch, me->u.u_f.fp) < 0)
    {
      assert (!"INTERNAL ERROR: failed ungetc!");
      abort (); /* ungetc should not be called without a previous read.  */
    }
  me->u.u_f.end = false;
  if (ch == '\n')
    start_of_input_line = false;
}

static void
file_clean (m4_input_block *me, m4 *context)
{
  if (me->prev)
    m4_debug_message (context, M4_DEBUG_TRACE_INPUT,
		      _("input reverted to %s, line %d"),
		      me->prev->file, me->prev->line);
  else
    m4_debug_message (context, M4_DEBUG_TRACE_INPUT, _("input exhausted"));

  if (ferror (me->u.u_f.fp))
    {
      m4_error (context, 0, 0, NULL, _("error reading file `%s'"), me->file);
      if (me->u.u_f.close)
	fclose (me->u.u_f.fp);
    }
  else if (me->u.u_f.close && fclose (me->u.u_f.fp) == EOF)
    m4_error (context, 0, errno, NULL, _("error reading file `%s'"), me->file);
  start_of_input_line = me->u.u_f.line_start;
  m4_set_output_line (context, -1);
}

static void
file_print (m4_input_block *me, m4 *context, m4_obstack *obs)
{
  const char *text = me->file;
  obstack_grow (obs, "<file: ", strlen ("<file: "));
  obstack_grow (obs, text, strlen (text));
  obstack_1grow (obs, '>');
}

/* m4_push_file () pushes an input file FP with name TITLE on the
  input stack, saving the current file name and line number.  If next
  is non-NULL, this push invalidates a call to m4_push_string_init (),
  whose storage is consequently released.  If CLOSE, then close FP at
  end of file.

  file_read () manages line numbers for error messages, so they do not
  get wrong due to lookahead.  The token consisting of a newline
  alone is taken as belonging to the line it ends, and the current
  line number is not incremented until the next character is read.  */
void
m4_push_file (m4 *context, FILE *fp, const char *title, bool close_file)
{
  m4_input_block *i;

  if (next != NULL)
    {
      obstack_free (current_input, next);
      next = NULL;
    }

  m4_debug_message (context, M4_DEBUG_TRACE_INPUT,
		    _("input read from %s"), title);

  i = (m4_input_block *) obstack_alloc (current_input, sizeof *i);
  i->funcs = &file_funcs;
  /* Save title on a separate obstack, so that wrapped text can refer
     to it even after the file is popped.  */
  i->file = obstack_copy0 (&file_names, title, strlen (title));
  i->line = 1;

  i->u.u_f.fp = fp;
  i->u.u_f.end = false;
  i->u.u_f.close = close_file;
  i->u.u_f.line_start = start_of_input_line;

  m4_set_output_line (context, -1);

  i->prev = isp;
  isp = i;
  input_change = true;
}


/* Handle a builtin macro token.  */
static int
builtin_peek (m4_input_block *me)
{
  if (me->u.u_b.read)
    return CHAR_RETRY;

  return CHAR_BUILTIN;
}

static int
builtin_read (m4_input_block *me, m4 *context M4_GNUC_UNUSED,
	      bool safe M4_GNUC_UNUSED)
{
  if (me->u.u_b.read)
    return CHAR_RETRY;

  me->u.u_b.read = true;
  return CHAR_BUILTIN;
}

static void
builtin_unget (m4_input_block *me, int ch)
{
  assert (ch == CHAR_BUILTIN && me->u.u_b.read);
  me->u.u_b.read = false;
}

static void
builtin_print (m4_input_block *me, m4 *context, m4_obstack *obs)
{
  const m4_builtin *bp = me->u.u_b.builtin;
  const char *text = bp->name;

  obstack_1grow (obs, '<');
  obstack_grow (obs, text, strlen (text));
  obstack_1grow (obs, '>');
  if (m4_is_debug_bit (context, M4_DEBUG_TRACE_MODULE))
    {
      text = m4_get_module_name (me->u.u_b.module);
      obstack_1grow (obs, '{');
      obstack_grow (obs, text, strlen (text));
      obstack_1grow (obs, '}');
    }
}

/* m4_push_builtin () pushes TOKEN, which contains a builtin's
   definition, on the input stack.  If next is non-NULL, this push
   invalidates a call to m4_push_string_init (), whose storage is
   consequently released.  */
void
m4_push_builtin (m4 *context, m4_symbol_value *token)
{
  m4_input_block *i;

  /* Make sure we were passed a builtin function type token.  */
  assert (m4_is_symbol_value_func (token));

  if (next != NULL)
    {
      obstack_free (current_input, next);
      next = NULL;
    }

  i = (m4_input_block *) obstack_alloc (current_input, sizeof *i);
  i->funcs = &builtin_funcs;
  i->file = m4_get_current_file (context);
  i->line = m4_get_current_line (context);

  i->u.u_b.builtin	= m4_get_symbol_value_builtin (token);
  i->u.u_b.module	= VALUE_MODULE (token);
  i->u.u_b.arg_signature = VALUE_ARG_SIGNATURE (token);
  i->u.u_b.flags	= VALUE_FLAGS (token);
  /* Check for bitfield truncation.  */
  assert (i->u.u_b.flags == VALUE_FLAGS (token)
	  && i->u.u_b.builtin->min_args == VALUE_MIN_ARGS (token)
	  && i->u.u_b.builtin->max_args == VALUE_MAX_ARGS (token));
  i->u.u_b.read		= false;

  i->prev = isp;
  isp = i;
  input_change = true;
}


/* Handle string expansion text.  */
static int
string_peek (m4_input_block *me)
{
  return me->u.u_s.len ? to_uchar (*me->u.u_s.str) : CHAR_RETRY;
}

static int
string_read (m4_input_block *me, m4 *context M4_GNUC_UNUSED,
	     bool safe M4_GNUC_UNUSED)
{
  if (!me->u.u_s.len)
    return CHAR_RETRY;
  me->u.u_s.len--;
  return to_uchar (*me->u.u_s.str++);
}

static void
string_unget (m4_input_block *me, int ch)
{
  assert (ch < CHAR_EOF && to_uchar (me->u.u_s.str[-1]) == ch);
  me->u.u_s.str--;
  me->u.u_s.len++;
}

static void
string_print (m4_input_block *me, m4 *context, m4_obstack *obs)
{
  bool quote = m4_is_debug_bit (context, M4_DEBUG_TRACE_QUOTE);
  size_t arg_length = m4_get_max_debug_arg_length_opt (context);

  m4_shipout_string_trunc (context, obs, me->u.u_s.str, me->u.u_s.len,
			   quote, &arg_length);
}

/* First half of m4_push_string ().  The pointer next points to the
   new input_block.  Return the obstack that will collect the
   expansion text.  */
m4_obstack *
m4_push_string_init (m4 *context)
{
  /* Free any memory occupied by completely parsed input.  */
  assert (!next);
  while (isp && pop_input (context, false));

  /* Reserve the next location on the obstack.  */
  next = (m4_input_block *) obstack_alloc (current_input, sizeof *next);
  next->funcs = &string_funcs;
  next->file = m4_get_current_file (context);
  next->line = m4_get_current_line (context);

  return current_input;
}

/* If VALUE contains text, then convert the current string into a
   chain if it is not one already, and add the contents of VALUE as a
   new link in the chain.  LEVEL describes the current expansion
   level, or SIZE_MAX if the contents of VALUE reside entirely on the
   current_input stack and VALUE lives in temporary storage.  Allows
   gathering input from multiple locations, rather than copying
   everything consecutively onto the input stack.  Must be called
   between push_string_init and push_string_finish.  */
void
m4__push_symbol (m4_symbol_value *value, size_t level)
{
  m4_symbol_chain *chain;

  assert (next);
  /* TODO - also accept TOKEN_COMP chains.  */
  assert (m4_is_symbol_value_text (value));
  if (m4_get_symbol_value_len (value) == 0)
    return;

  if (next->funcs == &string_funcs)
    {
      next->funcs = &composite_funcs;
      next->u.u_c.chain = next->u.u_c.end = NULL;
    }
  make_text_link (current_input, &next->u.u_c.chain, &next->u.u_c.end);
  chain = (m4_symbol_chain *) obstack_alloc (current_input, sizeof *chain);
  if (next->u.u_c.end)
    next->u.u_c.end->next = chain;
  else
    next->u.u_c.chain = chain;
  next->u.u_c.end = chain;
  chain->next = NULL;
  if (level != SIZE_MAX)
    /* TODO - use token as-is, rather than copying data.  This implies
       lengthening lifetime of $@ arguments until the rescan is
       complete, rather than the current approach of freeing them
       during expand_macro.  */
    chain->str = (char *) obstack_copy (current_input,
					m4_get_symbol_value_text (value),
					m4_get_symbol_value_len (value));
  else
    chain->str = m4_get_symbol_value_text (value);
  chain->len = m4_get_symbol_value_len (value);
  chain->argv = NULL;
  chain->index = 0;
  chain->flatten = false;
}

/* Last half of m4_push_string ().  If next is now NULL, a call to
   m4_push_file () or m4_push_builtin () has pushed a different input
   block to the top of the stack.  If the new object is void, we do
   not push it.  The function m4_push_string_finish () returns the
   opaque finished object, whether that is still a string or has been
   replaced by a file or builtin; this object can then be used in
   m4_input_print () during tracing.  This pointer is only for
   temporary use, since reading the next token might release the
   memory used for the object.  */
m4_input_block *
m4_push_string_finish (void)
{
  m4_input_block *ret = NULL;
  size_t len = obstack_object_size (current_input);

  if (next == NULL)
    {
      assert (!len);
      return isp;
    }

  if (len || next->funcs == &composite_funcs)
    {
      if (next->funcs == &string_funcs)
	{
	  next->u.u_s.str = (char *) obstack_finish (current_input);
	  next->u.u_s.len = len;
	}
      else
	make_text_link (current_input, &next->u.u_c.chain, &next->u.u_c.end);
      next->prev = isp;
      ret = isp = next;
      input_change = true;
    }
  else
    obstack_free (current_input, next);
  next = NULL;
  return ret;
}


/* A composite block contains multiple sub-blocks which are processed
   in FIFO order, even though the obstack allocates memory in LIFO
   order.  */
static int
composite_peek (m4_input_block *me)
{
  m4_symbol_chain *chain = me->u.u_c.chain;
  while (chain)
    {
      if (chain->str)
	{
	  if (chain->len)
	    return to_uchar (chain->str[0]);
	}
      else
	{
	  /* TODO - peek into argv.  */
	  assert (!"implemented yet");
	  abort ();
	}
      chain = chain->next;
    }
  return CHAR_RETRY;
}

static int
composite_read (m4_input_block *me, m4 *context, bool safe)
{
  m4_symbol_chain *chain = me->u.u_c.chain;
  while (chain)
    {
      if (chain->str)
	{
	  if (chain->len)
	    {
	      chain->len--;
	      return to_uchar (*chain->str++);
	    }
	}
      else
	{
	  /* TODO - peek into argv.  */
	  assert (!"implemented yet");
	  abort ();
	}
      if (safe)
	return CHAR_RETRY;
      me->u.u_c.chain = chain = chain->next;
    }
  return CHAR_RETRY;
}

static void
composite_unget (m4_input_block *me, int ch)
{
  m4_symbol_chain *chain = me->u.u_c.chain;
  if (chain->str)
    {
      assert (ch < CHAR_EOF && to_uchar (chain->str[-1]) == ch);
      chain->str--;
      chain->len++;
    }
  else
    {
      /* TODO support argv ref.  */
      assert (!"implemented yet");
      abort ();
    }
}

static void
composite_print (m4_input_block *me, m4 *context, m4_obstack *obs)
{
  bool quote = m4_is_debug_bit (context, M4_DEBUG_TRACE_QUOTE);
  size_t maxlen = m4_get_max_debug_arg_length_opt (context);
  m4_symbol_chain *chain = me->u.u_c.chain;
  const char *lquote = m4_get_syntax_lquote (M4SYNTAX);
  const char *rquote = m4_get_syntax_rquote (M4SYNTAX);

  if (quote)
    m4_shipout_string (context, obs, lquote, SIZE_MAX, false);
  while (chain)
    {
      /* TODO support argv refs as well.  */
      assert (chain->str);
      if (m4_shipout_string_trunc (context, obs, chain->str, chain->len, false,
				   &maxlen))
	break;
      chain = chain->next;
    }
  if (quote)
    m4_shipout_string (context, obs, rquote, SIZE_MAX, false);
}

/* Given an obstack OBS, capture any unfinished text as a link in the
   chain that starts at *START and ends at *END.  START may be NULL if
   *END is non-NULL.  */
static void
make_text_link (m4_obstack *obs, m4_symbol_chain **start,
		m4_symbol_chain **end)
{
  m4_symbol_chain *chain;
  size_t len = obstack_object_size (obs);

  assert (end && (start || *end));
  if (len)
    {
      char *str = (char *) obstack_finish (obs);
      chain = (m4_symbol_chain *) obstack_alloc (obs, sizeof *chain);
      if (*end)
	(*end)->next = chain;
      else
	*start = chain;
      *end = chain;
      chain->next = NULL;
      chain->str = str;
      chain->len = len;
      chain->argv = NULL;
      chain->index = 0;
      chain->flatten = false;
    }
}



/* When tracing, print a summary of the contents of the input block
   created by push_string_init/push_string_finish to OBS.  */
void
m4_input_print (m4 *context, m4_obstack *obs, m4_input_block *input)
{
  assert (context && obs);
  if (input == NULL)
    {
      bool quote = m4_is_debug_bit (context, M4_DEBUG_TRACE_QUOTE);
      if (quote)
	{
	  const char *lquote = m4_get_syntax_lquote (M4SYNTAX);
	  const char *rquote = m4_get_syntax_rquote (M4SYNTAX);
	  obstack_grow (obs, lquote, strlen (lquote));
	  obstack_grow (obs, rquote, strlen (rquote));
	}
    }
  else
    {
      assert (input->funcs->print_func);
      input->funcs->print_func (input, context, obs);
    }
}

/* The function m4_push_wrapup () pushes a string on the wrapup stack.
   When the normal input stack gets empty, the wrapup stack will become
   the input stack, and m4_push_string () and m4_push_file () will
   operate on wrapup_stack.  M4_push_wrapup should be done as
   m4_push_string (), but this will suffice, as long as arguments to
   m4_m4wrap () are moderate in size.

   FIXME - we should allow pushing builtins as well as text.  */
void
m4_push_wrapup (m4 *context, const char *s)
{
  m4_input_block *i;

  i = (m4_input_block *) obstack_alloc (wrapup_stack, sizeof *i);
  i->prev = wsp;

  i->funcs = &string_funcs;
  i->file = m4_get_current_file (context);
  i->line = m4_get_current_line (context);

  i->u.u_s.len = strlen (s);
  i->u.u_s.str = obstack_copy (wrapup_stack, s, i->u.u_s.len);

  wsp = i;
}


/* The function pop_input () pops one level of input sources.  If
   CLEANUP, the current_file and current_line are restored as needed.
   The return value is false if cleanup is still required, or if the
   current input source is not at the end.  */
static bool
pop_input (m4 *context, bool cleanup)
{
  m4_input_block *tmp = isp->prev;

  assert (isp);
  if (isp->funcs->peek_func (isp) != CHAR_RETRY)
    return false;

  if (isp->funcs->clean_func != NULL)
    {
      if (!cleanup)
	return false;
      isp->funcs->clean_func (isp, context);
    }

  if (tmp != NULL)
    {
      obstack_free (current_input, isp);
      next = NULL;	/* might be set in m4_push_string_init () */
    }

  isp = tmp;
  input_change = true;
  return true;
}

/* To switch input over to the wrapup stack, main () calls pop_wrapup.
   Since wrapup text can install new wrapup text, pop_wrapup ()
   returns true if there is more wrapped text to parse.  */
bool
m4_pop_wrapup (m4 *context)
{
  static size_t level = 0;

  next = NULL;
  obstack_free (current_input, NULL);
  free (current_input);

  if (wsp == NULL)
    {
      obstack_free (wrapup_stack, NULL);
      m4_set_current_file (context, NULL);
      m4_set_current_line (context, 0);
      m4_debug_message (context, M4_DEBUG_TRACE_INPUT,
		       _("input from m4wrap exhausted"));
      current_input = NULL;
      DELETE (wrapup_stack);
      return false;
    }

  m4_debug_message (context, M4_DEBUG_TRACE_INPUT,
		    _("input from m4wrap recursion level %lu"),
		    (unsigned long int) ++level);

  current_input = wrapup_stack;
  wrapup_stack = (m4_obstack *) xmalloc (sizeof *wrapup_stack);
  obstack_init (wrapup_stack);

  isp = wsp;
  wsp = NULL;
  input_change = true;

  return true;
}

/* When a BUILTIN token is seen, m4__next_token () uses init_builtin_token
   to retrieve the value of the function pointer.  */
static void
init_builtin_token (m4 *context, m4_symbol_value *token)
{
  m4_input_block *block = isp;
  assert (block->funcs->read_func == builtin_read && !block->u.u_b.read);

  m4_set_symbol_value_builtin (token, block->u.u_b.builtin);
  VALUE_MODULE (token)		= block->u.u_b.module;
  VALUE_FLAGS (token)		= block->u.u_b.flags;
  VALUE_ARG_SIGNATURE (token)	= block->u.u_b.arg_signature;
  VALUE_MIN_ARGS (token)	= block->u.u_b.builtin->min_args;
  VALUE_MAX_ARGS (token)	= block->u.u_b.builtin->max_args;
}


/* Low level input is done a character at a time.  The function
   next_char () is used to read and advance the input to the next
   character.  If RETRY, then avoid returning CHAR_RETRY by popping
   input.  */
static int
next_char (m4 *context, bool retry)
{
  int ch;

  while (1)
    {
      if (isp == NULL)
	{
	  m4_set_current_file (context, NULL);
	  m4_set_current_line (context, 0);
	  return CHAR_EOF;
	}

      if (input_change)
	{
	  m4_set_current_file (context, isp->file);
	  m4_set_current_line (context, isp->line);
	}

      assert (isp->funcs->read_func);
      while ((ch = isp->funcs->read_func (isp, context, !retry)) != CHAR_RETRY
	     || !retry)
	{
	  /* if (!IS_IGNORE (ch)) */
	  return ch;
	}

      /* End of input source --- pop one level.  */
      pop_input (context, true);
    }
}

/* The function peek_char () is used to look at the next character in
   the input stream.  At any given time, it reads from the input_block
   on the top of the current input stack.  */
static int
peek_char (m4 *context)
{
  int ch;
  m4_input_block *block = isp;

  while (1)
    {
      if (block == NULL)
	return CHAR_EOF;

      assert (block->funcs->peek_func);
      if ((ch = block->funcs->peek_func (block)) != CHAR_RETRY)
	{
	  return /* (IS_IGNORE (ch)) ? next_char (context, true) : */ ch;
	}

      block = block->prev;
    }
}

/* The function unget_input () puts back a character on the input
   stack, using an existing input_block if possible.  This is not safe
   to call except immediately after next_char(context, false).  */
static void
unget_input (int ch)
{
  assert (isp != NULL && isp->funcs->unget_func != NULL);
  isp->funcs->unget_func (isp, ch);
}

/* skip_line () simply discards all immediately following characters,
   up to the first newline.  It is only used from m4_dnl ().  NAME is
   the spelling of argv[0], for use in any warning message.  */
void
m4_skip_line (m4 *context, const char *name)
{
  int ch;
  const char *file = m4_get_current_file (context);
  int line = m4_get_current_line (context);

  while ((ch = next_char (context, true)) != CHAR_EOF && ch != '\n')
    ;
  if (ch == CHAR_EOF)
    /* current_file changed; use the previous value we cached.  */
    m4_warn_at_line (context, 0, file, line, name,
		     _("end of file treated as newline"));
  /* On the rare occasion that dnl crosses include file boundaries
     (either the input file did not end in a newline, or changesyntax
     was used), calling next_char can update current_file and
     current_line, and that update will be undone as we return to
     expand_macro.  This tells next_char () to restore the location.  */
  if (file != m4_get_current_file (context)
      || line != m4_get_current_line (context))
    input_change = true;
}


/* This function is for matching a string against a prefix of the
   input stream.  If the string S matches the input and CONSUME is
   true, the input is discarded; otherwise any characters read are
   pushed back again.  The function is used only when multicharacter
   quotes or comment delimiters are used.

   All strings herein should be unsigned.  Otherwise sign-extension
   of individual chars might break quotes with 8-bit chars in it.

   FIXME - when matching multiquotes that cross file boundaries, we do
   not properly restore the current input file and line when we
   restore unconsumed characters.  */
static bool
match_input (m4 *context, const char *s, bool consume)
{
  int n;			/* number of characters matched */
  int ch;			/* input character */
  const char *t;
  m4_obstack *st;
  bool result = false;

  ch = peek_char (context);
  if (ch != to_uchar (*s))
    return false;			/* fail */

  if (s[1] == '\0')
    {
      if (consume)
	next_char (context, true);
      return true;			/* short match */
    }

  next_char (context, true);
  for (n = 1, t = s++; (ch = peek_char (context)) == to_uchar (*s++); )
    {
      next_char (context, true);
      n++;
      if (*s == '\0')		/* long match */
	{
	  if (consume)
	    return true;
	  result = true;
	  break;
	}
    }

  /* Failed or shouldn't consume, push back input.  */
  st = m4_push_string_init (context);
  obstack_grow (st, t, n);
  m4_push_string_finish ();
  return result;
}

/* The macro MATCH() is used to match an unsigned char string S
  against the input.  The first character is handled inline, for
  speed.  Hopefully, this will not hurt efficiency too much when
  single character quotes and comment delimiters are used.  If
  CONSUME, then CH is the result of next_char, and a successful match
  will discard the matched string.  Otherwise, CH is the result of
  peek_char, and the input stream is effectively unchanged.  */
#define MATCH(C, ch, s, consume)					\
  (to_uchar ((s)[0]) == (ch)						\
   && (ch) != '\0'							\
   && ((s)[1] == '\0' || (match_input (C, (s) + (consume), consume))))

/* While the current input character has the given SYNTAX, append it
   to OBS.  Take care not to pop input source unless the next source
   would continue the chain.  Return true unless the chain ended with
   CHAR_EOF.  */
static bool
consume_syntax (m4 *context, m4_obstack *obs, unsigned int syntax)
{
  int ch;
  assert (syntax);
  while (1)
    {
      /* It is safe to call next_char without first checking
	 peek_char, except at input source boundaries, which we detect
	 by CHAR_RETRY.  We exploit the fact that CHAR_EOF and
	 CHAR_MACRO do not satisfy any syntax categories.  */
      while ((ch = next_char (context, false)) != CHAR_RETRY
	     && m4_has_syntax (M4SYNTAX, ch, syntax))
	obstack_1grow (obs, ch);
      if (ch == CHAR_RETRY)
	{
	  ch = peek_char (context);
	  if (m4_has_syntax (M4SYNTAX, ch, syntax))
	    {
	      obstack_1grow (obs, ch);
	      next_char (context, true);
	      continue;
	    }
	  return ch == CHAR_EOF;
	}
      unget_input (ch);
      return false;
    }
}


/* Inititialize input stacks.  */
void
m4_input_init (m4 *context)
{
  obstack_init (&file_names);
  m4_set_current_file (context, NULL);
  m4_set_current_line (context, 0);

  current_input = (m4_obstack *) xmalloc (sizeof *current_input);
  obstack_init (current_input);
  wrapup_stack = (m4_obstack *) xmalloc (sizeof *wrapup_stack);
  obstack_init (wrapup_stack);

  /* Allocate an object in the current chunk, so that obstack_free
     will always work even if the first token parsed spills to a new
     chunk.  */
  obstack_init (&token_stack);
  token_bottom = obstack_finish (&token_stack);

  isp = NULL;
  wsp = NULL;
  next = NULL;

  start_of_input_line = false;
}

/* Free memory used by the input engine.  */
void
m4_input_exit (void)
{
  assert (current_input == NULL);
  assert (wrapup_stack == NULL);
  obstack_free (&file_names, NULL);
  obstack_free (&token_stack, NULL);
}


/* Parse and return a single token from the input stream, built in
   TOKEN.  See m4__token_type for the valid return types, along with a
   description of what TOKEN will contain.  If LINE is not NULL, set
   *LINE to the line number where the token starts.  Report errors
   (unterminated comments or strings) on behalf of CALLER, if
   non-NULL.

   The token text is collected on the obstack token_stack, which never
   contains more than one token text at a time.  The storage pointed
   to by the fields in TOKEN is therefore subject to change the next
   time m4__next_token () is called.  */
m4__token_type
m4__next_token (m4 *context, m4_symbol_value *token, int *line,
		const char *caller)
{
  int ch;
  int quote_level;
  m4__token_type type;
  const char *file;
  int dummy;
  size_t len;

  assert (next == NULL);
  if (!line)
    line = &dummy;
  memset (token, '\0', sizeof *token);
  do {
    obstack_free (&token_stack, token_bottom);


    /* Must consume an input character, but not until CHAR_BUILTIN is
       handled.  */
    ch = peek_char (context);
    if (ch == CHAR_EOF)			/* EOF */
      {
#ifdef DEBUG_INPUT
	xfprintf (stderr, "next_token -> EOF\n");
#endif
	next_char (context, true);
	return M4_TOKEN_EOF;
      }

    if (ch == CHAR_BUILTIN)		/* BUILTIN TOKEN */
      {
	init_builtin_token (context, token);
	next_char (context, true);
#ifdef DEBUG_INPUT
	m4_print_token ("next_token", M4_TOKEN_MACDEF, token);
#endif
	return M4_TOKEN_MACDEF;
      }

    next_char (context, true); /* Consume character we already peeked at.  */
    file = m4_get_current_file (context);
    *line = m4_get_current_line (context);

    if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_ESCAPE))
      {					/* ESCAPED WORD */
	obstack_1grow (&token_stack, ch);
	if ((ch = next_char (context, true)) != CHAR_EOF)
	  {
	    obstack_1grow (&token_stack, ch);
	    if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_ALPHA))
	      consume_syntax (context, &token_stack,
			      M4_SYNTAX_ALPHA | M4_SYNTAX_NUM);
	    type = M4_TOKEN_WORD;
	  }
	else
	  type = M4_TOKEN_SIMPLE;	/* escape before eof */
      }
    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_ALPHA))
      {
	obstack_1grow (&token_stack, ch);
	consume_syntax (context, &token_stack,
			M4_SYNTAX_ALPHA | M4_SYNTAX_NUM);
	type = (m4_is_syntax_macro_escaped (M4SYNTAX)
		? M4_TOKEN_STRING : M4_TOKEN_WORD);
      }
    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_LQUOTE))
      {					/* QUOTED STRING, SINGLE QUOTES */
	quote_level = 1;
	while (1)
	  {
	    ch = next_char (context, true);
	    if (ch == CHAR_EOF)
	      m4_error_at_line (context, EXIT_FAILURE, 0, file, *line, caller,
				_("end of file in string"));

	    if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_RQUOTE))
	      {
		if (--quote_level == 0)
		  break;
		obstack_1grow (&token_stack, ch);
	      }
	    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_LQUOTE))
	      {
		quote_level++;
		obstack_1grow (&token_stack, ch);
	      }
	    else
	      obstack_1grow (&token_stack, ch);
	  }
	type = M4_TOKEN_STRING;
      }
    else if (!m4_is_syntax_single_quotes (M4SYNTAX)
	     && MATCH (context, ch, context->syntax->lquote.string, true))
      {					/* QUOTED STRING, LONGER QUOTES */
	quote_level = 1;
	while (1)
	  {
	    ch = next_char (context, true);
	    if (ch == CHAR_EOF)
	      m4_error_at_line (context, EXIT_FAILURE, 0, file, *line, caller,
				_("end of file in string"));
	    if (MATCH (context, ch, context->syntax->rquote.string, true))
	      {
		if (--quote_level == 0)
		  break;
		obstack_grow (&token_stack, context->syntax->rquote.string,
			      context->syntax->rquote.length);
	      }
	    else if (MATCH (context, ch, context->syntax->lquote.string, true))
	      {
		quote_level++;
		obstack_grow (&token_stack, context->syntax->lquote.string,
			      context->syntax->lquote.length);
	      }
	    else
	      obstack_1grow (&token_stack, ch);
	  }
	type = M4_TOKEN_STRING;
      }
    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_BCOMM))
      {					/* COMMENT, SHORT DELIM */
	obstack_1grow (&token_stack, ch);
	while ((ch = next_char (context, true)) != CHAR_EOF
	       && !m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_ECOMM))
	  obstack_1grow (&token_stack, ch);
	if (ch != CHAR_EOF)
	  obstack_1grow (&token_stack, ch);
	else
	  m4_error_at_line (context, EXIT_FAILURE, 0, file, *line, caller,
			    _("end of file in comment"));
	type = (m4_get_discard_comments_opt (context)
		? M4_TOKEN_NONE : M4_TOKEN_STRING);
      }
    else if (!m4_is_syntax_single_comments (M4SYNTAX)
	     && MATCH (context, ch, context->syntax->bcomm.string, true))
      {					/* COMMENT, LONGER DELIM */
	obstack_grow (&token_stack, context->syntax->bcomm.string,
		      context->syntax->bcomm.length);
	while ((ch = next_char (context, true)) != CHAR_EOF
	       && !MATCH (context, ch, context->syntax->ecomm.string, true))
	  obstack_1grow (&token_stack, ch);
	if (ch != CHAR_EOF)
	  obstack_grow (&token_stack, context->syntax->ecomm.string,
			context->syntax->ecomm.length);
	else
	  m4_error_at_line (context, EXIT_FAILURE, 0, file, *line, caller,
			    _("end of file in comment"));
	type = (m4_get_discard_comments_opt (context)
		? M4_TOKEN_NONE : M4_TOKEN_STRING);
      }
    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_ACTIVE))
      {					/* ACTIVE CHARACTER */
	obstack_1grow (&token_stack, ch);
	type = M4_TOKEN_WORD;
      }
    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_OPEN))
      {					/* OPEN PARENTHESIS */
	obstack_1grow (&token_stack, ch);
	type = M4_TOKEN_OPEN;
      }
    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_COMMA))
      {					/* COMMA */
	obstack_1grow (&token_stack, ch);
	type = M4_TOKEN_COMMA;
      }
    else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_CLOSE))
      {					/* CLOSE PARENTHESIS */
	obstack_1grow (&token_stack, ch);
	type = M4_TOKEN_CLOSE;
      }
    else if (m4_is_syntax_single_quotes (M4SYNTAX)
	     && m4_is_syntax_single_comments (M4SYNTAX))
      {			/* EVERYTHING ELSE (SHORT QUOTES AND COMMENTS) */
	obstack_1grow (&token_stack, ch);

	if (m4_has_syntax (M4SYNTAX, ch,
			   (M4_SYNTAX_OTHER | M4_SYNTAX_NUM | M4_SYNTAX_DOLLAR
			    | M4_SYNTAX_LBRACE | M4_SYNTAX_RBRACE)))
	  {
	    consume_syntax (context, &token_stack,
			    (M4_SYNTAX_OTHER | M4_SYNTAX_NUM
			     | M4_SYNTAX_DOLLAR | M4_SYNTAX_LBRACE
			     | M4_SYNTAX_RBRACE));
	    type = M4_TOKEN_STRING;
	  }
	else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_SPACE))
	  {
	    /* Coalescing newlines when interactive or when synclines
	       are enabled is wrong.  */
	    if (!m4_get_interactive_opt (context)
		&& !m4_get_syncoutput_opt (context))
	      consume_syntax (context, &token_stack, M4_SYNTAX_SPACE);
	    type = M4_TOKEN_SPACE;
	  }
	else
	  type = M4_TOKEN_SIMPLE;
      }
    else		/* EVERYTHING ELSE (LONG QUOTES OR COMMENTS) */
      {
	obstack_1grow (&token_stack, ch);

	if (m4_has_syntax (M4SYNTAX, ch,
			   (M4_SYNTAX_OTHER | M4_SYNTAX_NUM | M4_SYNTAX_DOLLAR
			    | M4_SYNTAX_LBRACE | M4_SYNTAX_RBRACE)))
	  type = M4_TOKEN_STRING;
	else if (m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_SPACE))
	  type = M4_TOKEN_SPACE;
	else
	  type = M4_TOKEN_SIMPLE;
      }
  } while (type == M4_TOKEN_NONE);

  len = obstack_object_size (&token_stack);
  obstack_1grow (&token_stack, '\0');

  m4_set_symbol_value_text (token, obstack_finish (&token_stack), len,
			    m4__quote_age (M4SYNTAX));
  VALUE_MAX_ARGS (token)	= -1;

#ifdef DEBUG_INPUT
  m4_print_token ("next_token", type, token);
#endif

  return type;
}

/* Peek at the next token in the input stream to see if it is an open
   parenthesis.  It is possible that what is peeked at may change as a
   result of changequote (or friends).  This honors multi-character
   comments and quotes, just as next_token does.  */
bool
m4__next_token_is_open (m4 *context)
{
  int ch = peek_char (context);

  if (ch == CHAR_EOF || ch == CHAR_BUILTIN
      || m4_has_syntax (M4SYNTAX, ch, (M4_SYNTAX_BCOMM | M4_SYNTAX_ESCAPE
				       | M4_SYNTAX_ALPHA | M4_SYNTAX_LQUOTE
				       | M4_SYNTAX_ACTIVE))
      || (!m4_is_syntax_single_comments (M4SYNTAX)
	  && MATCH (context, ch, context->syntax->bcomm.string, false))
      || (!m4_is_syntax_single_quotes (M4SYNTAX)
	  && MATCH (context, ch, context->syntax->lquote.string, false)))
    return false;
  return m4_has_syntax (M4SYNTAX, ch, M4_SYNTAX_OPEN);
}


#ifdef DEBUG_INPUT

int
m4_print_token (const char *s, m4__token_type type, m4_symbol_value *token)
{
  xfprintf (stderr, "%s: ", s ? s : "m4input");
  switch (type)
    {				/* TOKSW */
    case M4_TOKEN_EOF:
      xfprintf (stderr, "eof\n");
      break;
    case M4_TOKEN_NONE:
      xfprintf (stderr, "none\n");
      break;
    case M4_TOKEN_STRING:
      xfprintf (stderr, "string\t\"%s\"\n", m4_get_symbol_value_text (token));
      break;
    case M4_TOKEN_SPACE:
      xfprintf (stderr, "space\t\"%s\"\n", m4_get_symbol_value_text (token));
      break;
    case M4_TOKEN_WORD:
      xfprintf (stderr, "word\t\"%s\"\n", m4_get_symbol_value_text (token));
      break;
    case M4_TOKEN_OPEN:
      xfprintf (stderr, "open\t\"%s\"\n", m4_get_symbol_value_text (token));
      break;
    case M4_TOKEN_COMMA:
      xfprintf (stderr, "comma\t\"%s\"\n", m4_get_symbol_value_text (token));
      break;
    case M4_TOKEN_CLOSE:
      xfprintf (stderr, "close\t\"%s\"\n", m4_get_symbol_value_text (token));
      break;
    case M4_TOKEN_SIMPLE:
      xfprintf (stderr, "simple\t\"%s\"\n", m4_get_symbol_value_text (token));
      break;
    case M4_TOKEN_MACDEF:
      {
	const m4_builtin *bp;
	bp = m4_builtin_find_by_func (NULL, m4_get_symbol_value_func (token));
	assert (bp);
	xfprintf (stderr, "builtin\t<%s>{%s}\n", bp->name,
		  m4_get_module_name (VALUE_MODULE (token)));
      }
      break;
    }
  return 0;
}
#endif /* DEBUG_INPUT */
