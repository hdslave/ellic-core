# ellic-core

Melodee's engine and model, shared by Ellic Studio's products: Melodee on the
MPC Sample ([melodee-sample](https://github.com/keremimo/melodee-sample)) and
on Ableton Move ([melodee-move](https://github.com/keremimo/melodee-move)).

A product adds this repository as its `core/` submodule and includes
`core.mk`, which lists the sources and the flags they need. See
[AGENTS.md](AGENTS.md) for what belongs here and what does not.

Melodee is by Kerem Kiliç (Ellic Studio). The core's own code is under the
MIT licence ([LICENSE](LICENSE)). It is built with JUCE, which has licences
of its own: see [THIRD_PARTY.md](THIRD_PARTY.md) before shipping a binary.
