/*
 *  VHDL code generation for statements.
 *
 *  Copyright (C) 2008  Nick Gasson (nick@nickg.me.uk)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "vhdl_target.h"

#include <iostream>
#include <cstring>
#include <cassert>
#include <sstream>
#include <typeinfo>

/*
 * VHDL has no real equivalent of Verilog's $finish task. The
 * current solution is to use `assert false ...' to terminate
 * the simulator. This isn't great, as the simulator will
 * return a failure exit code when in fact it completed
 * successfully.
 *
 * An alternative is to use the VHPI interface supported by
 * some VHDL simulators and implement the $finish funcitonality
 * in C. This function can be enabled with the flag
 * -puse-vhpi-finish=1. 
 */
static int draw_stask_finish(vhdl_procedural *proc, stmt_container *container,
                             ivl_statement_t stmt)
{
   const char *use_vhpi = ivl_design_flag(get_vhdl_design(), "use-vhpi-finish");
   if (strcmp(use_vhpi, "1") == 0) {
      //get_active_entity()->requires_package("work.Verilog_Support");
      container->add_stmt(new vhdl_pcall_stmt("work.Verilog_Support.Finish"));
   }
   else {
      container->add_stmt(new vhdl_assert_stmt("SIMULATION FINISHED"));
   }
   
   return 0;
}

/*
 * Generate VHDL for system tasks (like $display). Not all of
 * these are supported.
 */
static int draw_stask(vhdl_procedural *proc, stmt_container *container,
                      ivl_statement_t stmt)
{
   const char *name = ivl_stmt_name(stmt);

   if (strcmp(name, "$display") == 0)
      return draw_stask_display(proc, container, stmt, true);
   else if (strcmp(name, "$write") == 0)
      return draw_stask_display(proc, container, stmt, false);
   else if (strcmp(name, "$finish") == 0)
      return draw_stask_finish(proc, container, stmt);
   else {
      vhdl_seq_stmt *result = new vhdl_null_stmt();
      ostringstream ss;
      ss << "Unsupported system task " << name << " omitted here ("
         << ivl_stmt_file(stmt) << ":" << ivl_stmt_lineno(stmt) << ")";
      result->set_comment(ss.str());
      container->add_stmt(result);
      cerr << "Warning: no VHDL translation for system task " << name << endl;
      return 0;
   }
}

/*
 * Generate VHDL for a block of Verilog statements. This doesn't
 * actually do anything, other than recursively translate the
 * block's statements and add them to the process. This is OK as
 * the stmt_container class behaves like a Verilog block.
 */
static int draw_block(vhdl_procedural *proc, stmt_container *container,
                      ivl_statement_t stmt, bool is_last)
{
   int count = ivl_stmt_block_count(stmt);
   for (int i = 0; i < count; i++) {
      ivl_statement_t stmt_i = ivl_stmt_block_stmt(stmt, i);
      if (draw_stmt(proc, container, stmt_i, is_last && i == count - 1) != 0)
         return 1;
   }
   return 0;
}

/*
 * A no-op statement. This corresponds to a `null' statement in VHDL.
 */
static int draw_noop(vhdl_procedural *proc, stmt_container *container,
                     ivl_statement_t stmt)
{
   container->add_stmt(new vhdl_null_stmt());
   return 0;
}

static vhdl_var_ref *make_assign_lhs(ivl_lval_t lval, vhdl_scope *scope)
{
   ivl_signal_t sig = ivl_lval_sig(lval);
   if (!sig) {
      error("Only signals as lvals supported at the moment");
      return NULL;
   }

   vhdl_expr *base = NULL;
   ivl_expr_t e_off = ivl_lval_part_off(lval);
   if (NULL == e_off)
      e_off = ivl_lval_idx(lval);
   if (e_off) {
      if ((base = translate_expr(e_off)) == NULL)
         return NULL;
         
      vhdl_type integer(VHDL_TYPE_INTEGER);
      base = base->cast(&integer);
   }
   
   unsigned lval_width = ivl_lval_width(lval);
   
   string signame(get_renamed_signal(sig));
   vhdl_decl *decl = scope->get_decl(signame);
   assert(decl);
   
   vhdl_type *ltype = new vhdl_type(*decl->get_type());
   vhdl_var_ref *lval_ref = new vhdl_var_ref(signame.c_str(), ltype);
   if (base) {
      if (decl->get_type()->get_name() == VHDL_TYPE_ARRAY)
         lval_ref->set_slice(base, 0);
      else
         lval_ref->set_slice(base, lval_width - 1);
   }

   return lval_ref;
}

static bool assignment_lvals(ivl_statement_t stmt, vhdl_procedural *proc,
                             list<vhdl_var_ref*> &lvals)
{
   int nlvals = ivl_stmt_lvals(stmt);
   for (int i = 0; i < nlvals; i++) {
      ivl_lval_t lval = ivl_stmt_lval(stmt, i);
      vhdl_var_ref *lhs = make_assign_lhs(lval, proc->get_scope());
      if (NULL == lhs)
         return false;

      lvals.push_back(lhs);
   }         

   return true;
}

/*
 * Generate an assignment of type T for the Verilog statement stmt.
 */
template <class T>
void make_assignment(vhdl_procedural *proc, stmt_container *container,
                     ivl_statement_t stmt, bool blocking)
{
   list<vhdl_var_ref*> lvals;
   if (!assignment_lvals(stmt, proc, lvals))
      return;

   vhdl_expr *rhs, *rhs2 = NULL;
   ivl_expr_t rval = ivl_stmt_rval(stmt);
   if (ivl_expr_type(rval) == IVL_EX_TERNARY) {
      rhs = translate_expr(ivl_expr_oper2(rval));
      rhs2 = translate_expr(ivl_expr_oper3(rval));
   }
   else
      rhs = translate_expr(rval);
   if (rhs == NULL)
      return;

   if (lvals.size() == 1) {
      vhdl_var_ref *lhs = lvals.front();
      rhs = rhs->cast(lhs->get_type());
     
      ivl_expr_t i_delay;
      vhdl_expr *after = NULL;
      if ((i_delay = ivl_stmt_delay_expr(stmt)) != NULL)
         after = translate_time_expr(i_delay);
      
      // A small optimisation is to expand ternary RHSs into an
      // if statement (eliminates a function call and produces
      // more idiomatic code)
      if (ivl_expr_type(rval) == IVL_EX_TERNARY) {
         rhs2 = rhs2->cast(lhs->get_type());
         vhdl_var_ref *lhs2 =
            make_assign_lhs(ivl_stmt_lval(stmt, 0), proc->get_scope());
         
         vhdl_expr *test = translate_expr(ivl_expr_oper1(rval));
         if (NULL == test)
            return;

         vhdl_if_stmt *vhdif = new vhdl_if_stmt(test);

         // True part
         {
            T *a = new T(lhs, rhs);
            if (after)
               a->set_after(after);
            vhdif->get_then_container()->add_stmt(a);
         }

         // False part
         {
            T *a = new T(lhs2, rhs2);
            if (after)
               a->set_after(translate_time_expr(i_delay));
            vhdif->get_else_container()->add_stmt(a);
         }

         container->add_stmt(vhdif);
         return;
      }

      // Where possible, move constant assignments into the
      // declaration as initializers. This optimisation is only
      // performed on assignments of constant values to prevent
      // ordering problems.
      
      // This also has another application: If this is an `inital'
      // process and we haven't yet generated a `wait' statement then
      // moving the assignment to the initialization preserves the
      // expected Verilog behaviour: VHDL does not distinguish
      // `initial' and `always' processes so an `always' process might
      // be activatated before an `initial' process at time 0. The
      // `always' process may then use the uninitialized signal value.
      // The second test ensures that we only try to initialise
      // internal signals not ports
      ivl_lval_t lval = ivl_stmt_lval(stmt, 0);
      vhdl_decl *decl = proc->get_scope()->get_decl(lhs->get_name());
      if (proc->get_scope()->initializing()
          && ivl_signal_port(ivl_lval_sig(lval)) == IVL_SIP_NONE
          && !decl->has_initial()
          && rhs->constant()
          && decl->get_type()->get_name() != VHDL_TYPE_ARRAY) {
            
         // If this assignment is not in the top-level container
         // it will not be made on all paths through the code
         // This precludes any future extraction of an initialiser
         if (container != proc->get_container())
            decl->set_initial(NULL);   // Default initial value
         else {
            decl->set_initial(rhs);
            delete lhs;
            return;
         }
      }
      
      T *a = new T(lhs, rhs);
      container->add_stmt(a);
          
      if (after != NULL)
         a->set_after(after);
   }
   else {
      // Multiple lvals are implemented by first assigning the complete
      // RHS to a temporary, and then assigning each lval in turn as
      // bit-selects of the temporary

      static int tmp_count = 0;
      ostringstream ss;
      ss << "Verilog_Assign_Tmp_" << tmp_count++;
      string tmpname = ss.str();
      
      proc->get_scope()->add_decl
         (new vhdl_var_decl(tmpname.c_str(), new vhdl_type(*rhs->get_type())));

      vhdl_var_ref *tmp_ref =
         new vhdl_var_ref(tmpname.c_str(), new vhdl_type(*rhs->get_type()));
      container->add_stmt(new vhdl_assign_stmt(tmp_ref, rhs));
      
      list<vhdl_var_ref*>::iterator it;
      int width_so_far = 0;
      for (it = lvals.begin(); it != lvals.end(); ++it) {
         vhdl_var_ref *tmp_rhs =
            new vhdl_var_ref(tmpname.c_str(), new vhdl_type(*rhs->get_type()));
         
         int lval_width = (*it)->get_type()->get_width();
         vhdl_expr *slice_base = new vhdl_const_int(width_so_far);
         tmp_rhs->set_slice(slice_base, lval_width - 1);
         
         ivl_expr_t i_delay;
         vhdl_expr *after = NULL;
         if ((i_delay = ivl_stmt_delay_expr(stmt)) != NULL)
            after = translate_time_expr(i_delay);

         T *a = new T(*it, tmp_rhs);
         if (after)
            a->set_after(after);

         container->add_stmt(a);

         width_so_far += lval_width;
      }
   }
}

/*
 * A non-blocking assignment inside a process. The semantics for
 * this are essentially the same as VHDL's non-blocking signal
 * assignment.
 */
static int draw_nbassign(vhdl_procedural *proc, stmt_container *container,
                         ivl_statement_t stmt)
{
   assert(proc->get_scope()->allow_signal_assignment());

   make_assignment<vhdl_nbassign_stmt>(proc, container, stmt, false);

   return 0;
}

static int draw_assign(vhdl_procedural *proc, stmt_container *container,
                       ivl_statement_t stmt, bool is_last)
{
   if (proc->get_scope()->allow_signal_assignment()) {
      // Blocking assignment is implemented as non-blocking assignment
      // followed by a zero-time wait
      // This follows the Verilog semantics fairly closely.

      make_assignment<vhdl_nbassign_stmt>(proc, container, stmt, false);

      // Don't generate a zero-wait if this is the last statement in
      // the process
      if (!is_last)
         container->add_stmt
            (new vhdl_wait_stmt(VHDL_WAIT_FOR, new vhdl_const_time(0)));
   }
   else
      make_assignment<vhdl_assign_stmt>(proc, container, stmt, true);
   
   return 0;
}

/*
 * Delay statements are equivalent to the `wait for' form of the
 * VHDL wait statement.
 */
static int draw_delay(vhdl_procedural *proc, stmt_container *container,
                      ivl_statement_t stmt)
{
   // This currently ignores the time units and precision
   // of the enclosing scope
   // A neat way to do this would be to make these values
   // constants in the scope (type is Time), and have the
   // VHDL wait statement compute the value from that.
   // The other solution is to add them as parameters to
   // the vhdl_process class
   vhdl_expr *time;
   if (ivl_statement_type(stmt) == IVL_ST_DELAY) {
      uint64_t value = ivl_stmt_delay_val(stmt);
      time = new vhdl_const_time(value, TIME_UNIT_NS);
   }
   else {
      time = translate_time_expr(ivl_stmt_delay_expr(stmt));
      if (NULL == time)
         return 1;
   }

   ivl_statement_t sub_stmt = ivl_stmt_sub_stmt(stmt);
   vhdl_wait_stmt *wait =
      new vhdl_wait_stmt(VHDL_WAIT_FOR, time);
   container->add_stmt(wait);

   // Expand the sub-statement as well
   // Often this would result in a useless `null' statement which
   // is caught here instead
   if (ivl_statement_type(sub_stmt) != IVL_ST_NOOP)
      draw_stmt(proc, container, sub_stmt);
   
   // Any further assignments occur after simulation time 0
   // so they cannot be used to initialize signal declarations
   // (if this scope is an initial process)
   proc->get_scope()->set_initializing(false);
   
   return 0;
}

/*
 * A wait statement waits for a level change on a @(..) list of
 * signals. Purely combinatorial processes (i.e. no posedge/negedge
 * events) produce a `wait on' statement at the end of the process.
 * Sequential processes produce a `wait until' statement at the
 * start of the process.
 */
static int draw_wait(vhdl_procedural *_proc, stmt_container *container,
                     ivl_statement_t stmt)
{
   // Wait statements only occur in processes
   vhdl_process *proc = dynamic_cast<vhdl_process*>(_proc);
   assert(proc);   // Catch not process

   vhdl_binop_expr *test =
      new vhdl_binop_expr(VHDL_BINOP_OR, vhdl_type::boolean());
   
   int nevents = ivl_stmt_nevent(stmt);
   
   bool combinatorial = true;  // True if no negedge/posedge events
   for (int i = 0; i < nevents; i++) {
      ivl_event_t event = ivl_stmt_events(stmt, i);
      if (ivl_event_npos(event) > 0 || ivl_event_nneg(event) > 0)
         combinatorial = false;
   }

   if (combinatorial) {
      vhdl_wait_stmt *wait = new vhdl_wait_stmt(VHDL_WAIT_ON); 
      
      for (int i = 0; i < nevents; i++) {
         ivl_event_t event = ivl_stmt_events(stmt, i);
         
         int nany = ivl_event_nany(event);
         for (int i = 0; i < nany; i++) {
            ivl_nexus_t nexus = ivl_event_any(event, i);
            vhdl_var_ref *ref = nexus_to_var_ref(proc->get_scope(), nexus);

            wait->add_sensitivity(ref->get_name());
            delete ref;
         }
      }

      draw_stmt(proc, container, ivl_stmt_sub_stmt(stmt), true);
      container->add_stmt(wait);
   }
   else {
      for (int i = 0; i < nevents; i++) {
         ivl_event_t event = ivl_stmt_events(stmt, i);
         
         int nany = ivl_event_nany(event);
         for (int i = 0; i < nany; i++) {
            ivl_nexus_t nexus = ivl_event_any(event, i);
            vhdl_var_ref *ref = nexus_to_var_ref(proc->get_scope(), nexus);
            
            ref->set_name(ref->get_name() + "'Event");
            test->add_expr(ref);
         }
         
         int nneg = ivl_event_nneg(event);
         for (int i = 0; i < nneg; i++) {
            ivl_nexus_t nexus = ivl_event_neg(event, i);
            vhdl_var_ref *ref = nexus_to_var_ref(proc->get_scope(), nexus);
            vhdl_fcall *detect =
               new vhdl_fcall("falling_edge", vhdl_type::boolean());
            detect->add_expr(ref);
            
            test->add_expr(detect);
         }
         
         int npos = ivl_event_npos(event);
         for (int i = 0; i < npos; i++) {
            ivl_nexus_t nexus = ivl_event_pos(event, i);
            vhdl_var_ref *ref = nexus_to_var_ref(proc->get_scope(), nexus);
            vhdl_fcall *detect =
               new vhdl_fcall("rising_edge", vhdl_type::boolean());
            detect->add_expr(ref);
         
            test->add_expr(detect);
         }
      }
      
      container->add_stmt(new vhdl_wait_stmt(VHDL_WAIT_UNTIL, test));
      draw_stmt(proc, container, ivl_stmt_sub_stmt(stmt), true);
   }
   
   return 0;
}

static int draw_if(vhdl_procedural *proc, stmt_container *container,
                   ivl_statement_t stmt, bool is_last)
{
   vhdl_expr *test = translate_expr(ivl_stmt_cond_expr(stmt));
   if (NULL == test)
      return 1;
   
   vhdl_if_stmt *vhdif = new vhdl_if_stmt(test);

   ivl_statement_t cond_true_stmt = ivl_stmt_cond_true(stmt);
   if (cond_true_stmt)
      draw_stmt(proc, vhdif->get_then_container(), cond_true_stmt, is_last);

   ivl_statement_t cond_false_stmt = ivl_stmt_cond_false(stmt);
   if (cond_false_stmt)
      draw_stmt(proc, vhdif->get_else_container(), cond_false_stmt, is_last);

   container->add_stmt(vhdif);
   
   return 0;
}

static int draw_case(vhdl_procedural *proc, stmt_container *container,
                     ivl_statement_t stmt, bool is_last)
{
   vhdl_expr *test = translate_expr(ivl_stmt_cond_expr(stmt));
   if (NULL == test)
      return 1;

   // VHDL case expressions are required to be quite simple: variable
   // references or slices. So we may need to create a temporary
   // variable to hold the result of the expression evaluation
   if (typeid(*test) != typeid(vhdl_var_ref)) {
      const char *tmp_name = "Verilog_Case_Ex";
      vhdl_type *test_type = new vhdl_type(*test->get_type());
         
      if (!proc->get_scope()->have_declared(tmp_name)) {
         proc->get_scope()->add_decl
            (new vhdl_var_decl(tmp_name, new vhdl_type(*test_type)));
      }

      vhdl_var_ref *tmp_ref = new vhdl_var_ref(tmp_name, NULL);
      container->add_stmt(new vhdl_assign_stmt(tmp_ref, test));

      test = new vhdl_var_ref(tmp_name, test_type);
   }
   
   vhdl_case_stmt *vhdlcase = new vhdl_case_stmt(test);
   container->add_stmt(vhdlcase);

   // VHDL is more strict than Verilog about covering every
   // possible case. So make sure we add an 'others' branch
   // if there isn't a default one.
   bool have_others = false;
   
   int nbranches = ivl_stmt_case_count(stmt);
   for (int i = 0; i < nbranches; i++) {
      vhdl_expr *when;
      ivl_expr_t net = ivl_stmt_case_expr(stmt, i);
      if (net) {
         when = translate_expr(net)->cast(test->get_type());
         if (NULL == when)
            return 1;
      }
      else {
         when = new vhdl_var_ref("others", NULL);
         have_others = true;
      }
      
      vhdl_case_branch *branch = new vhdl_case_branch(when);
      vhdlcase->add_branch(branch);

      ivl_statement_t stmt_i = ivl_stmt_case_stmt(stmt, i);
      draw_stmt(proc, branch->get_container(), stmt_i, is_last);
   }

   if (!have_others) {
      vhdl_case_branch *others =
         new vhdl_case_branch(new vhdl_var_ref("others", NULL));
      others->get_container()->add_stmt(new vhdl_null_stmt());
      vhdlcase->add_branch(others);
   }      
   
   return 0;
}

int draw_while(vhdl_procedural *proc, stmt_container *container,
               ivl_statement_t stmt)
{
   vhdl_expr *test = translate_expr(ivl_stmt_cond_expr(stmt));
   if (NULL == test)
      return 1;

   // The test must be a Boolean (and std_logic and (un)signed types
   // must be explicitly cast unlike in Verilog)
   vhdl_type boolean(VHDL_TYPE_BOOLEAN);
   test = test->cast(&boolean);

   vhdl_while_stmt *loop = new vhdl_while_stmt(test);
   container->add_stmt(loop);

   draw_stmt(proc, loop->get_container(), ivl_stmt_sub_stmt(stmt));
   
   return 0;
}

int draw_forever(vhdl_procedural *proc, stmt_container *container,
                 ivl_statement_t stmt)
{
   vhdl_loop_stmt *loop = new vhdl_loop_stmt;
   container->add_stmt(loop);

   draw_stmt(proc, loop->get_container(), ivl_stmt_sub_stmt(stmt));

   return 0;
}

int draw_repeat(vhdl_procedural *proc, stmt_container *container,
                ivl_statement_t stmt)
{
   vhdl_expr *times = translate_expr(ivl_stmt_cond_expr(stmt));
   if (NULL == times)
      return 1;

   vhdl_type integer(VHDL_TYPE_INTEGER);
   times = times->cast(&integer);

   const char *it_name = "Verilog_Repeat";
   vhdl_for_stmt *loop =
      new vhdl_for_stmt(it_name, new vhdl_const_int(1), times);
   container->add_stmt(loop);

   draw_stmt(proc, loop->get_container(), ivl_stmt_sub_stmt(stmt));
   
   return 0;
}

/*
 * Tasks are difficult to translate to VHDL since they allow things
 * not allowed by VHDL's corresponding procedures (e.g. updating
 * global variables. The solution here is to expand tasks in-line.
 */
int draw_utask(vhdl_procedural *proc, stmt_container *container,
               ivl_statement_t stmt)
{
   ivl_scope_t tscope = ivl_stmt_call(stmt);

   // TODO: adding some comments to the output would be helpful

   // TOOD: this completely ignores paremeters!
   draw_stmt(proc, container, ivl_scope_def(tscope), false);
   
   return 0;
}

/*
 * Generate VHDL statements for the given Verilog statement and
 * add them to the given VHDL process. The container is the
 * location to add statements: e.g. the process body, a branch
 * of an if statement, etc.
 *
 * The flag is_last should be set if this is the final statement
 * in a block or process. It avoids generating useless `wait for 0ns'
 * statements if the next statement would be a wait anyway.
 */
int draw_stmt(vhdl_procedural *proc, stmt_container *container,
              ivl_statement_t stmt, bool is_last)
{
   assert(stmt);
   
   switch (ivl_statement_type(stmt)) {
   case IVL_ST_STASK:
      return draw_stask(proc, container, stmt);
   case IVL_ST_BLOCK:
      return draw_block(proc, container, stmt, is_last);
   case IVL_ST_NOOP:
      return draw_noop(proc, container, stmt);
   case IVL_ST_ASSIGN:
      return draw_assign(proc, container, stmt, is_last);
   case IVL_ST_ASSIGN_NB:
      return draw_nbassign(proc, container, stmt);
   case IVL_ST_DELAY:
   case IVL_ST_DELAYX:
      return draw_delay(proc, container, stmt);
   case IVL_ST_WAIT:
      return draw_wait(proc, container, stmt);
   case IVL_ST_CONDIT:
      return draw_if(proc, container, stmt, is_last);
   case IVL_ST_CASE:
      return draw_case(proc, container, stmt, is_last);
   case IVL_ST_WHILE:
      return draw_while(proc, container, stmt);
   case IVL_ST_FOREVER:
      return draw_forever(proc, container, stmt);
   case IVL_ST_REPEAT:
      return draw_repeat(proc, container, stmt);
   case IVL_ST_UTASK:
      return draw_utask(proc, container, stmt);
   case IVL_ST_FORCE:
   case IVL_ST_RELEASE:
      error("force/release statements cannot be translated to VHDL");
      return 1;
   case IVL_ST_DISABLE:
      error("disable statement cannot be translated to VHDL");
      return 1;
   case IVL_ST_CASEX:
      error("casex statement cannot be translated to VHDL");
      return 1;
   case IVL_ST_CASEZ:
      error("casez statement cannot be translated to VHDL");
      return 1;
   case IVL_ST_FORK:
      error("fork statement cannot be translated to VHDL");
      return 1;
   case IVL_ST_CASSIGN:
   case IVL_ST_DEASSIGN:
      error("continuous procedural assignment cannot be translated to VHDL");
      return 1;
   default:
      error("No VHDL translation for statement at %s:%d (type = %d)",
            ivl_stmt_file(stmt), ivl_stmt_lineno(stmt),
            ivl_statement_type(stmt));
      return 1;            
   }
}
