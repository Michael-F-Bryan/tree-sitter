#include "spec_helper.h"
#include "helpers/tree_helpers.h"
#include "helpers/record_alloc.h"
#include "helpers/stream_methods.h"
#include "runtime/stack.h"
#include "runtime/tree.h"
#include "runtime/length.h"
#include "runtime/alloc.h"

enum {
  stateA = 1,
  stateB,
  stateC, stateD, stateE, stateF, stateG, stateH, stateI, stateJ
};

enum {
  symbol0, symbol1, symbol2, symbol3, symbol4, symbol5, symbol6, symbol7, symbol8,
  symbol9, symbol10
};

TSLength operator*(const TSLength &length, size_t factor) {
  return {length.bytes * factor, length.chars * factor, 0, length.columns * factor};
}

void free_slice_array(StackSliceArray *slices) {
  for (size_t i = 0; i < slices->size; i++) {
    StackSlice slice = slices->contents[i];

    bool matches_prior_trees = false;
    for (size_t j = 0; j < i; j++) {
      StackSlice prior_slice = slices->contents[j];
      if (slice.trees.contents == prior_slice.trees.contents) {
        matches_prior_trees = true;
        break;
      }
    }

    if (!matches_prior_trees) {
      for (size_t j = 0; j < slice.trees.size; j++)
        ts_tree_release(slice.trees.contents[j]);
      array_delete(&slice.trees);
    }
  }
}

struct StackEntry {
  TSStateId state;
  size_t depth;
};

vector<StackEntry> get_stack_entries(Stack *stack, StackVersion version) {
  vector<StackEntry> result;
  ts_stack_iterate(
    stack,
    version,
    [](void *payload, TSStateId state, TreeArray *trees, size_t tree_count, bool is_done, bool is_pending) -> StackIterateAction {
      auto entries = static_cast<vector<StackEntry> *>(payload);
      StackEntry entry = {state, tree_count};
      if (find(entries->begin(), entries->end(), entry) == entries->end())
        entries->push_back(entry);
      return StackIterateNone;
    }, &result);
  return result;
}

START_TEST

describe("Stack", [&]() {
  Stack *stack;
  const size_t tree_count = 11;
  TSTree *trees[tree_count];
  TSLength tree_len = {2, 3, 0, 3};

  before_each([&]() {
    record_alloc::start();

    stack = ts_stack_new();

    for (size_t i = 0; i < tree_count; i++)
      trees[i] = ts_tree_make_leaf(i, ts_length_zero(), tree_len, {
        true, true, false, true,
      });
  });

  after_each([&]() {
    ts_stack_delete(stack);
    for (size_t i = 0; i < tree_count; i++)
      ts_tree_release(trees[i]);

    record_alloc::stop();
    AssertThat(record_alloc::outstanding_allocation_indices(), IsEmpty());
  });

  describe("push(version, tree, is_pending, state)", [&]() {
    it("adds entries to the given version of the stack", [&]() {
      AssertThat(ts_stack_version_count(stack), Equals<size_t>(1));
      AssertThat(ts_stack_top_state(stack, 0), Equals(0));
      AssertThat(ts_stack_top_position(stack, 0), Equals(ts_length_zero()));

      // . <──0── A*
      ts_stack_push(stack, 0, trees[0], false, stateA);
      AssertThat(ts_stack_top_state(stack, 0), Equals(stateA));
      AssertThat(ts_stack_top_position(stack, 0), Equals(tree_len));

      // . <──0── A <──1── B*
      ts_stack_push(stack, 0, trees[1], false, stateB);
      AssertThat(ts_stack_top_state(stack, 0), Equals(stateB));
      AssertThat(ts_stack_top_position(stack, 0), Equals(tree_len * 2));

      // . <──0── A <──1── B <──2── C*
      ts_stack_push(stack, 0, trees[2], false, stateC);
      AssertThat(ts_stack_top_state(stack, 0), Equals(stateC));
      AssertThat(ts_stack_top_position(stack, 0), Equals(tree_len * 3));

      AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
        {stateC, 0},
        {stateB, 1},
        {stateA, 2},
        {0, 3},
      })));
    });
  });

  describe("merge()", [&]() {
    before_each([&]() {
      // . <──0── A <──1── B*
      //          ↑
      //          └───2─── C*
      ts_stack_push(stack, 0, trees[0], false, stateA);
      ts_stack_pop_count(stack, 0, 0);
      ts_stack_push(stack, 0, trees[1], false, stateB);
      ts_stack_push(stack, 1, trees[2], false, stateC);
    });

    it("combines versions that have the same top states and positions", [&]() {
      // . <──0── A <──1── B <──3── D*
      //          ↑
      //          └───2─── C <──4── D*
      ts_stack_push(stack, 0, trees[3], false, stateD);
      ts_stack_push(stack, 1, trees[4], false, stateD);

      // . <──0── A <──1── B <──3── D*
      //          ↑                 |
      //          └───2─── C <──4───┘
      ts_stack_merge(stack);
      AssertThat(ts_stack_version_count(stack), Equals<size_t>(1));
      AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
        {stateD, 0},
        {stateB, 1},
        {stateC, 1},
        {stateA, 2},
        {0, 3},
      })));
    });

    it("does not combine versions that have different states", [&]() {
      ts_stack_merge(stack);
      AssertThat(ts_stack_version_count(stack), Equals<size_t>(2));
    });

    it("does not combine versions that have different positions", [&]() {
      // . <──0── A <──1── B <────3──── D*
      //          ↑
      //          └───2─── C <──4── D*
      trees[3]->size = tree_len * 3;
      ts_stack_push(stack, 0, trees[3], false, stateD);
      ts_stack_push(stack, 1, trees[4], false, stateD);

      ts_stack_merge(stack);
      AssertThat(ts_stack_version_count(stack), Equals<size_t>(2));
    });

    describe("when the merged versions have more than one common entry", [&]() {
      it("combines all of the top common entries", [&]() {
        // . <──0── A <──1── B <──3── D <──5── E*
        //          ↑
        //          └───2─── C <──4── D <──5── E*
        ts_stack_push(stack, 0, trees[3], false, stateD);
        ts_stack_push(stack, 0, trees[5], false, stateE);
        ts_stack_push(stack, 1, trees[4], false, stateD);
        ts_stack_push(stack, 1, trees[5], false, stateE);

        // . <──0── A <──1── B <──3── D <──5── E*
        //          ↑                 |
        //          └───2─── C <──4───┘
        ts_stack_merge(stack);
        AssertThat(ts_stack_version_count(stack), Equals<size_t>(1));
        AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
          {stateE, 0},
          {stateD, 1},
          {stateB, 2},
          {stateC, 2},
          {stateA, 3},
          {0, 4},
        })));
      });
    });
  });

  describe("pop_count(version, count)", [&]() {
    before_each([&]() {
      // . <──0── A <──1── B <──2── C*
      ts_stack_push(stack, 0, trees[0], false, stateA);
      ts_stack_push(stack, 0, trees[1], false, stateB);
      ts_stack_push(stack, 0, trees[2], false, stateC);
    });

    it("creates a new version with the given number of entries removed", [&]() {
      // . <──0── A <──1── B <──2── C*
      //          ↑
      //          └─*
      StackPopResult pop = ts_stack_pop_count(stack, 0, 2);
      AssertThat(pop.status, Equals(StackPopResult::StackPopSucceeded));
      AssertThat(pop.slices.size, Equals<size_t>(1));
      AssertThat(ts_stack_version_count(stack), Equals<size_t>(2));

      StackSlice slice = pop.slices.contents[0];
      AssertThat(slice.version, Equals<StackVersion>(1));
      AssertThat(slice.trees, Equals(vector<TSTree *>({ trees[1], trees[2] })));
      AssertThat(ts_stack_top_state(stack, 1), Equals(stateA));

      free_slice_array(&pop.slices);
    });

    it("does not count 'extra' trees toward the given count", [&]() {
      trees[1]->extra = true;

      // . <──0── A <──1── B <──2── C*
      // ↑
      // └─*
      StackPopResult pop = ts_stack_pop_count(stack, 0, 2);
      AssertThat(pop.status, Equals(StackPopResult::StackPopSucceeded));
      AssertThat(pop.slices.size, Equals<size_t>(1));

      StackSlice slice = pop.slices.contents[0];
      AssertThat(slice.trees, Equals(vector<TSTree *>({ trees[0], trees[1], trees[2] })));
      AssertThat(ts_stack_top_state(stack, 1), Equals(0));

      free_slice_array(&pop.slices);
    });

    it("stops popping entries early if it reaches an error tree", [&]() {
      // . <──0── A <──1── B <──2── C <──3── ERROR <──4── D*
      ts_stack_push(stack, 0, trees[3], false, ts_parse_state_error);
      ts_stack_push(stack, 0, trees[4], false, stateD);

      // . <──0── A <──1── B <──2── C <──3── ERROR <──4── D*
      //                                       ↑
      //                                       └─*
      StackPopResult pop = ts_stack_pop_count(stack, 0, 3);
      AssertThat(pop.status, Equals(StackPopResult::StackPopStoppedAtError));

      AssertThat(ts_stack_version_count(stack), Equals<size_t>(2));
      AssertThat(ts_stack_top_state(stack, 1), Equals(ts_parse_state_error));

      AssertThat(pop.slices.size, Equals<size_t>(1));
      StackSlice slice = pop.slices.contents[0];
      AssertThat(slice.version, Equals<StackVersion>(1));
      AssertThat(slice.trees, Equals(vector<TSTree *>({ trees[4] })));

      free_slice_array(&pop.slices);
    });

    describe("when the version has been merged", [&]() {
      before_each([&]() {
        // . <──0── A <──1── B <──2── C <──3── D <──10── I*
        //          ↑                          |
        //          └───4─── E <──5── F <──6───┘
        ts_stack_push(stack, 0, trees[3], false, stateD);
        StackPopResult pop = ts_stack_pop_count(stack, 0, 3);
        free_slice_array(&pop.slices);
        ts_stack_push(stack, 1, trees[4], false, stateE);
        ts_stack_push(stack, 1, trees[5], false, stateF);
        ts_stack_push(stack, 1, trees[6], false, stateD);
        ts_stack_merge(stack);
        ts_stack_push(stack, 0, trees[10], false, stateI);

        AssertThat(ts_stack_version_count(stack), Equals<size_t>(1));
        AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
          {stateI, 0},
          {stateD, 1},
          {stateC, 2},
          {stateF, 2},
          {stateB, 3},
          {stateE, 3},
          {stateA, 4},
          {0, 5},
        })));
      });

      describe("when there are two paths that reveal different versions", [&]() {
        it("returns an entry for each revealed version", [&]() {
          // . <──0── A <──1── B <──2── C <──3── D <──10── I*
          //          ↑        ↑
          //          |        └*
          //          |
          //          └───4─── E*
          StackPopResult pop = ts_stack_pop_count(stack, 0, 3);
          AssertThat(pop.slices.size, Equals<size_t>(2));

          StackSlice slice1 = pop.slices.contents[0];
          AssertThat(slice1.version, Equals<StackVersion>(1));
          AssertThat(slice1.trees, Equals(vector<TSTree *>({ trees[2], trees[3], trees[10] })));

          StackSlice slice2 = pop.slices.contents[1];
          AssertThat(slice2.version, Equals<StackVersion>(2));
          AssertThat(slice2.trees, Equals(vector<TSTree *>({ trees[5], trees[6], trees[10] })));

          AssertThat(ts_stack_version_count(stack), Equals<size_t>(3));
          AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
            {stateI, 0},
            {stateD, 1},
            {stateC, 2},
            {stateF, 2},
            {stateB, 3},
            {stateE, 3},
            {stateA, 4},
            {0, 5},
          })));
          AssertThat(get_stack_entries(stack, 1), Equals(vector<StackEntry>({
            {stateB, 0},
            {stateA, 1},
            {0, 2},
          })));
          AssertThat(get_stack_entries(stack, 2), Equals(vector<StackEntry>({
            {stateE, 0},
            {stateA, 1},
            {0, 2},
          })));

          free_slice_array(&pop.slices);
        });
      });

      describe("when there is one path that ends at a merged version", [&]() {
        it("returns a single entry", [&]() {
          // . <──0── A <──1── B <──2── C <──3── D <──10── I*
          //          |                          |
          //          └───5─── F <──6── G <──7───┘
          //                                     |
          //                                     └*
          StackPopResult pop = ts_stack_pop_count(stack, 0, 1);
          AssertThat(pop.slices.size, Equals<size_t>(1));

          StackSlice slice1 = pop.slices.contents[0];
          AssertThat(slice1.version, Equals<StackVersion>(1));
          AssertThat(slice1.trees, Equals(vector<TSTree *>({ trees[10] })));

          AssertThat(ts_stack_version_count(stack), Equals<size_t>(2));
          AssertThat(ts_stack_top_state(stack, 0), Equals(stateI));
          AssertThat(ts_stack_top_state(stack, 1), Equals(stateD));

          free_slice_array(&pop.slices);
        });
      });

      describe("when there are two paths that converge on one version", [&]() {
        it("returns two slices with the same version", [&]() {
          // . <──0── A <──1── B <──2── C <──3── D <──10── I*
          //          ↑                          |
          //          ├───4─── E <──5── F <──6───┘
          //          |
          //          └*
          StackPopResult pop = ts_stack_pop_count(stack, 0, 4);
          AssertThat(pop.slices.size, Equals<size_t>(2));

          StackSlice slice1 = pop.slices.contents[0];
          AssertThat(slice1.version, Equals<StackVersion>(1));
          AssertThat(slice1.trees, Equals(vector<TSTree *>({ trees[1], trees[2], trees[3], trees[10] })));

          StackSlice slice2 = pop.slices.contents[1];
          AssertThat(slice2.version, Equals<StackVersion>(1));
          AssertThat(slice2.trees, Equals(vector<TSTree *>({ trees[4], trees[5], trees[6], trees[10] })))

          AssertThat(ts_stack_version_count(stack), Equals<size_t>(2));
          AssertThat(ts_stack_top_state(stack, 0), Equals(stateI));
          AssertThat(ts_stack_top_state(stack, 1), Equals(stateA));

          free_slice_array(&pop.slices);
        });
      });

      describe("when there are three paths that lead to three different versions", [&]() {
        it("returns three entries with different arrays of trees", [&]() {
          // . <──0── A <──1── B <──2── C <──3── D <──10── I*
          //          ↑                          |
          //          ├───4─── E <──5── F <──6───┘
          //          |                          |
          //          └───7─── G <──8── H <──9───┘
          StackPopResult pop = ts_stack_pop_count(stack, 0, 4);
          free_slice_array(&pop.slices);
          ts_stack_push(stack, 1, trees[7], false, stateG);
          ts_stack_push(stack, 1, trees[8], false, stateH);
          ts_stack_push(stack, 1, trees[9], false, stateD);
          ts_stack_push(stack, 1, trees[10], false, stateI);
          ts_stack_merge(stack);

          AssertThat(ts_stack_version_count(stack), Equals<size_t>(1));
          AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
            {stateI, 0},
            {stateD, 1},
            {stateC, 2},
            {stateF, 2},
            {stateH, 2},
            {stateB, 3},
            {stateE, 3},
            {stateG, 3},
            {stateA, 4},
            {0, 5},
          })));

          // . <──0── A <──1── B <──2── C <──3── D <──10── I*
          //          ↑                 ↑
          //          |                 └*
          //          |
          //          ├───4─── E <──5── F*
          //          |
          //          └───7─── G <──8── H*
          pop = ts_stack_pop_count(stack, 0, 2);
          AssertThat(pop.slices.size, Equals<size_t>(3));

          StackSlice slice1 = pop.slices.contents[0];
          AssertThat(slice1.version, Equals<StackVersion>(1));
          AssertThat(slice1.trees, Equals(vector<TSTree *>({ trees[3], trees[10] })))

          StackSlice slice2 = pop.slices.contents[1];
          AssertThat(slice2.version, Equals<StackVersion>(2));
          AssertThat(slice2.trees, Equals(vector<TSTree *>({ trees[6], trees[10] })))

          StackSlice slice3 = pop.slices.contents[2];
          AssertThat(slice3.version, Equals<StackVersion>(3));
          AssertThat(slice3.trees, Equals(vector<TSTree *>({ trees[9], trees[10] })))

          AssertThat(ts_stack_version_count(stack), Equals<size_t>(4));
          AssertThat(ts_stack_top_state(stack, 0), Equals(stateI));
          AssertThat(ts_stack_top_state(stack, 1), Equals(stateC));
          AssertThat(ts_stack_top_state(stack, 2), Equals(stateF));
          AssertThat(ts_stack_top_state(stack, 3), Equals(stateH));

          free_slice_array(&pop.slices);
        });
      });
    });
  });

  describe("pop_pending(version)", [&]() {
    before_each([&]() {
      ts_stack_push(stack, 0, trees[0], false, stateA);
    });

    it("removes the top node from the stack if it was pushed in pending mode", [&]() {
      ts_stack_push(stack, 0, trees[1], true, stateB);

      StackPopResult pop = ts_stack_pop_pending(stack, 0);
      AssertThat(pop.status, Equals(StackPopResult::StackPopSucceeded));
      AssertThat(pop.slices.size, Equals<size_t>(1));

      AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
        {stateA, 0},
        {0, 1},
      })));

      free_slice_array(&pop.slices);
    });

    it("does nothing if the top node was not pushed in pending mode", [&]() {
      ts_stack_push(stack, 0, trees[1], false, stateB);

      StackPopResult pop = ts_stack_pop_pending(stack, 0);
      AssertThat(pop.status, Equals(StackPopResult::StackPopSucceeded));
      AssertThat(pop.slices.size, Equals<size_t>(0));

      AssertThat(get_stack_entries(stack, 0), Equals(vector<StackEntry>({
        {stateB, 0},
        {stateA, 1},
        {0, 2},
      })));

      free_slice_array(&pop.slices);
    });
  });
});

END_TEST

bool operator==(const StackEntry &left, const StackEntry &right) {
  return left.state == right.state && left.depth == right.depth;
}

std::ostream &operator<<(std::ostream &stream, const StackEntry &entry) {
  return stream << "{" << entry.state << ", " << entry.depth << "}";
}

std::ostream &operator<<(std::ostream &stream, const TreeArray &array) {
  stream << "[";
  bool first = true;
  for (size_t i = 0; i < array.size; i++) {
    if (!first)
      stream << ", ";
    first = false;
    stream << array.contents[i];
  }
  return stream << "]";
}
