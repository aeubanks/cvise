from cvise.passes.abstract import PassResult, ProcessEventNotifier

def iterate_pass(current_pass, path):
    state = current_pass.new(path)
    while state is not None:
        (result, state) = current_pass.transform(path, state, ProcessEventNotifier(None))
        if result == PassResult.OK:
            state = current_pass.advance_on_success(path, state)
        else:
            state = current_pass.advance(path, state)
