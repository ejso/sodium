/**
 * Copyright (c) 2012, Stephen Blackheath and Anthony Jones
 * All rights reserved.
 *
 * Released under a BSD3 licence.
 *
 * C++ implementation courtesy of International Telematics Ltd.
 */
#include <sodium/sodium.h>

using namespace std;
using namespace boost;


namespace sodium {
#define GCC_VERSION (__GNUC__ * 10000 \
                   + __GNUC_MINOR__ * 100 \
                   + __GNUC_PATCHLEVEL__)

#if GCC_VERSION < 40700
#define WORKAROUND_GCC_46_BUG
#endif

    namespace impl {
#ifdef WORKAROUND_GCC_46_BUG
        struct Nulllistener {
            std::function<void()> operator () (transaction*, const std::shared_ptr<impl::node>&,
                        const std::function<void(transaction*, const light_ptr&)>&,
                        const std::shared_ptr<cleaner_upper>&) {
                return [] () {};
            }
        };
        
        // Save a bit of memory by having one global instance of the 'never' listener.
        static const Nulllistener& getNever()
        {
            static Nulllistener* l;
            if (l == NULL)
                l = new Nulllistener;
            return *l;
        }
    
        event_::event_()
            : listen_impl_(getNever())
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            , is_never_(true)
#endif
        {
        }
#else  // !WORKAROUND_GCC_46_BUG
        // Save a bit of memory by having one global instance of the 'never' listener.
        static const impl::event_::listen& getNever()
        {
            static impl::event_::listen* l;
            if (l == NULL)
                l = new impl::event_::listen(
                    [] (, const std::shared_ptr<impl::node>&,
                        const std::function<void(, const light_ptr&)>&,
                        const std::shared_ptr<cleaner_upper>&)
                    {
                        return [] () {};
                    }
                );
            return *l;
        }

        event_::event_()
            : listen_impl_(getNever())
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            , is_never_(true)
#endif
        {
        }
#endif  // WORKAROUND_GCC_46_BUG
    
        event_::event_(const listen& listen_impl_)
            : listen_impl_(listen_impl_)
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            , is_never_(false)
#endif
        {
        }
    
        event_::event_(const listen& listen_impl_, const std::shared_ptr<cleaner_upper>& cleanerUpper
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
                , bool is_never_
#endif
            )
            : listen_impl_(listen_impl_), cleanerUpper(cleanerUpper)
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            , is_never_(is_never_)
#endif
        {
        }
    
        event_::event_(const listen& listen_impl_, const std::function<void()>& f
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
                , bool is_never_
#endif
            )
            : listen_impl_(listen_impl_), cleanerUpper(new cleaner_upper(f))
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            , is_never_(is_never_)
#endif
        {
        }
    
        /*!
         * listen to events.
         */
        std::function<void()> event_::listen_raw_(
                    transaction* trans0,
                    const std::shared_ptr<impl::node>& target,
                    const std::function<void(transaction*, const light_ptr&)>& handle) const
        {
            return listen_impl_(trans0, target, handle, cleanerUpper);
        }

        void touch(const cleaner_upper&)
        {
        }

        /*!
         * The specified cleanup is performed whenever nobody is referencing this event
         * any more.
         */
        event_ event_::add_cleanup(const std::function<void()>& newCleanup) const {
            const std::shared_ptr<cleaner_upper>& cleanerUpper = this->cleanerUpper;
            if (cleanerUpper)
                return event_(listen_impl_, [newCleanup, cleanerUpper] () {
                    newCleanup();
                    touch(*cleanerUpper);  // Keep the reference to the old clean-up
                }
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
                , is_never_
#endif
                );
            else
                return event_(listen_impl_, newCleanup
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
                    , is_never_
#endif
                    );
        }

        struct holder {
            holder(
                const std::function<void(transaction*, const light_ptr&)>& handle,
                const std::shared_ptr<cleaner_upper>& cleanerUpper
            ) : handle(handle), cleanerUpper(cleanerUpper) {}
            std::function<void(transaction*, const light_ptr&)> handle;
            std::shared_ptr<cleaner_upper> cleanerUpper;
        };

        behavior_impl::behavior_impl()
            : seq(NULL),
              changes(event_()),
              sample([] () { return boost::optional<light_ptr>(); })
        {
        }

        behavior_impl::behavior_impl(const light_ptr& constant)
            : seq(NULL),
              changes(event_()),
              sample([constant] () { return boost::optional<light_ptr>(constant); })
        {
        }

        behavior_impl::behavior_impl(
            sch::Sequence* seq,
            const event_& changes,
            const std::function<boost::optional<light_ptr>()>& sample)
        : seq(seq), changes(changes), sample(sample) {}

#ifdef WORKAROUND_GCC_46_BUG
        struct new_event_listener {
            new_event_listener(const std::shared_ptr<node>& node) : node(node) {}
            std::function<void()> operator () (
                    transaction* trans,
                    const std::shared_ptr<impl::node>& target,
                    const std::function<void(transaction*, const light_ptr&)>& handle,
                    const std::shared_ptr<cleaner_upper>& cleanerUpper) {
                const partition<untyped>& part = trans.partition();
                std::list<light_ptr> firings;
                void* h = new holder(handle, cleanerUpper);
                {
                    MutexLock ml(part._impl_mutex());       
                    node->link(h, target);     
                    firings = node->firings;
                }
                if (firings.begin() != firings.end())
                    sch::mkevent<void>(part.sequence(), [trans, handle, firings] () {
                        for (auto it = firings.begin(); it != firings.end(); it++)
                            handle(trans, *it);
                    })->schedule();
                const std::shared_ptr<node>& node(this->node);
                return [node, part, h] () {  // Unregister listener
                    MutexLock ml(part._impl_mutex());
                    if (node->unlink(h)) {
                        if (part.sequence()->isInContext())
                            delete (holder*)h;
                        else {
                            sch::mkevent<void>(part.sequence(), [h] () {
                                delete (holder*)h;
                            })->schedule();
                        }
                    }
                };
            }

            std::shared_ptr<node> node;
        };

        /*!
         * Creates an event, and a function to push a value into it.
         * Unsafe variant: Assumes 'push' is called on the partition's sequence.
         */
        std::tuple<event_, std::function<void(transaction*, const light_ptr&)>, std::shared_ptr<node>> unsafe_new_event()
        {
            std::shared_ptr<node> node(new node);
            return std::make_tuple(
                // The event
                event_(new_event_listener(node)),
#else
        /*!
         * Creates an event, and a function to push a value into it.
         * Unsafe variant: Assumes 'push' is called on the partition's sequence.
         */
        std::tuple<event_, std::function<void(transaction*, const light_ptr&)>, std::shared_ptr<node>> unsafe_new_event()
        {
            std::shared_ptr<node> node(new node);
            return std::make_tuple(
                // The event
                event_(
                    [node] (transaction* trans,
                            const std::shared_ptr<node>& target,
                            const std::function<void(transaction*, const light_ptr&)>& handle,
                            const std::shared_ptr<cleaner_upper>& cleanerUpper) {  // Register listener
                        const partition<untyped>& part = trans.partition();
                        std::list<light_ptr> firings;
                        void* h = new holder(handle, cleanerUpper);
                        {
                            MutexLock ml(part._impl_mutex());
                            node->link(h, target);     
                            firings = node->firings;
                        }
                        if (firings.begin() != firings.end())
                            sch::mkevent<void>(part.sequence(), [trans, handle, firings] () {
                                for (auto it = firings.begin(); it != firings.end(); it++)
                                    handle(trans, *it);
                            })->schedule();
                        return [node, part, h] () {  // Unregister listener
                            MutexLock ml(part._impl_mutex());
                            if (node->unlink(h)) {
                                if (part.sequence()->isInContext())
                                    delete (holder*)h;
                                else {
                                    sch::mkevent<void>(part.sequence(), [h] () {
                                        delete (holder*)h;
                                    })->schedule();
                                }
                            }
                        };
                    }
                ),
#endif
                // Function to push a value into this event
                [node] (transaction* trans, const light_ptr& ptr) {
                    const partition<untyped>& part = trans.partition();
                    int ifs = 0;
                    holder* fs[16];
                    std::list<holder*> fsOverflow;
                    {
                        MutexLock ml(part._impl_mutex());
                        if (node->firings.begin() == node->firings.end())
                            trans.last([node] (long long id) {
                                node->firings.clear();
                            });
                        node->firings.push_back(ptr);
                        auto it = node->targets.begin();
                        while (it != node->targets.end()) {
                            fs[ifs++] = (holder*)it->handler;
                            it++;
                            if (ifs == 16) {
                                while (it != node->targets.end()) {
                                    fsOverflow.push_back((holder*)it->handler);
                                    it++;
                                }
                                break;
                            }
                        }
                    }
                    for (int i = 0; i < ifs; i++)
                        fs[i]->handle(trans, ptr);
                    for (auto it = fsOverflow.begin(); it != fsOverflow.end(); ++it)
                        (*it)->handle(trans, ptr);
                },
                node
            );
        }

        behavior_state::behavior_state(const boost::optional<light_ptr>& initA)
            : current(initA)
        {
        }
        
        behavior_state::~behavior_state()
        {
        }

        behavior_impl* hold(transaction* trans0, const light_ptr& initValue, const event_& input)
        {
            auto p = unsafe_new_event();
            auto out = std::get<0>(p);
            auto push = std::get<1>(p);
            auto target = std::get<2>(p);
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            if (input.is_never())
                return new behavior_impl(NULL, input, [initValue] () { return initValue; });
            else {
#endif
                std::shared_ptr<behavior_state> state(new behavior_state(initValue));
                auto unlisten = input.listen_raw_(trans0, target,
                                                 [push, state] (transaction* trans, const light_ptr& ptr) {
                    bool first = !state->update;
                    state->update = boost::optional<light_ptr>(ptr);
                    if (first)
                        trans.last([state] (long long tid) {
                            state->current = state->update;
                            state->update = boost::optional<light_ptr>();
                        });
                    push(trans, ptr);
                });
                auto changes = out.add_cleanup(unlisten);
                auto sample = [state] () { return state->current ? state->current : state->update; };
                return new behavior_impl(trans0.partition().sequence(), changes, sample);
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            }
#endif
        }

        behavior_::behavior_()
            : impl(new behavior_impl)
        {
        }

        behavior_::behavior_(behavior_impl* impl)
            : impl(impl)
        {
        }

        behavior_::behavior_(const std::shared_ptr<behavior_impl>& impl)
            : impl(impl)
        {
        }

        behavior_::behavior_(const light_ptr& a)
            : impl(new behavior_impl(a))
        {
        }

        behavior_::behavior_(
            sch::Sequence* seq,
            const event_& changes,
            const std::function<boost::optional<light_ptr>()>& sample
        )
            : impl(new behavior_impl(seq, changes, sample))
        {
        }

#if defined(SODIUM_CONSTANT_OPTIMIZATION)
        /*!
         * For optimization, if this behavior is a constant, then return its value.
         */
        boost::optional<light_ptr> behavior_::getConstantValue() const
        {
            return impl->changes.is_never() ? impl->sample()
                                                   : boost::optional<light_ptr>();
        }
#endif

        /* Clean up the listener so if there are multiple firings per transaction, they're
           combined into one. */
        std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                            const std::function<void(transaction*, const light_ptr&)>&,
                                            const std::shared_ptr<cleaner_upper>&)>
            coalesce_with_cu_impl(
                const std::function<light_ptr(const light_ptr&, const light_ptr&)>& combine,
                const std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                const std::function<void(transaction*, const light_ptr&)>&,
                                const std::shared_ptr<cleaner_upper>&)>& listen_raw
            )
        {
            return [combine, listen_raw] (transaction* trans, const std::shared_ptr<node>& target,
                                const std::function<void(transaction*, const light_ptr&)>& handle,
                                const std::shared_ptr<cleaner_upper>& cleanerUpper)
                                                                            -> std::function<void()> {
                std::shared_ptr<coalesce_state> pState(new coalesce_state);
                return listen_raw(trans, target, [handle, combine, pState, target] (transaction* trans, const light_ptr& ptr) {
                    if (!pState->oValue) {
                        pState->oValue = boost::optional<light_ptr>(ptr);
                        trans.prioritized(rankOf(target), [handle, pState] (const std::shared_ptr<transaction_impl>& impl) {
                            if (pState->oValue) {
                                transaction resurrected(impl);
                                handle(resurrected, pState->oValue.get());
                                pState->oValue = boost::optional<light_ptr>();
                            }
                        });
                    }
                    else
                        pState->oValue = make_optional(combine(pState->oValue.get(), ptr));
                }, cleanerUpper);
            };
        }
        
        /* Clean up the listener so if there are multiple firings per transaction, they're
           combined into one. */
        std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                            const std::function<void(transaction*, const light_ptr&)>&)>
            coalesce_with_impl(
                const std::function<light_ptr(const light_ptr&, const light_ptr&)>& combine,
                const std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                const std::function<void(transaction*, const light_ptr&)>&)>& listen_raw
            )
        {
            return [combine, listen_raw] (transaction* trans, const std::shared_ptr<node>& target,
                                const std::function<void(transaction*, const light_ptr&)>& handle)
                                                                            -> std::function<void()> {
                std::shared_ptr<coalesce_state> pState(new coalesce_state);
                return listen_raw(trans, target, [handle, combine, pState, target] (transaction* trans, const light_ptr& ptr) {
                    if (!pState->oValue) {
                        pState->oValue = boost::optional<light_ptr>(ptr);
                        trans.prioritized(rankOf(target), [handle, pState] (const std::shared_ptr<transaction_impl>& impl) {
                            if (pState->oValue) {
                                transaction resurrected(impl);
                                handle(resurrected, pState->oValue.get());
                                pState->oValue = boost::optional<light_ptr>();
                            }
                        });
                    }
                    else
                        pState->oValue = make_optional(combine(pState->oValue.get(), ptr));
                });
            };
        }

        std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                            const std::function<void(transaction*, const light_ptr&)>&)>
            coalesce(
                const std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                                    const std::function<void(transaction*, const light_ptr&)>&)>& listen_raw
            )
        {
            return [listen_raw] (transaction* trans, const std::shared_ptr<node>& target,
                                const std::function<void(transaction*, const light_ptr&)>& handle) -> std::function<void()> {
                std::shared_ptr<coalesce_state> pState(new coalesce_state);
                return listen_raw(trans, target, [handle, pState, target] (transaction* trans, const light_ptr& ptr) {
                    bool first = !(bool)pState->oValue;
                    pState->oValue = boost::optional<light_ptr>(ptr);
                    if (first)
                        trans.prioritized(rankOf(target), [handle, pState] (const std::shared_ptr<transaction_impl>& impl) {
                            if (pState->oValue) {
                                transaction resurrected(impl);
                                handle(resurrected, pState->oValue.get());
                                pState->oValue = boost::optional<light_ptr>();
                            }
                        });
                });
            };
        }

        std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                            const std::function<void(transaction*, const light_ptr&)>&,
                                            const std::shared_ptr<cleaner_upper>&)>
            coalesce_cu(
                const std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                                                    const std::function<void(transaction*, const light_ptr&)>&,
                                                    const std::shared_ptr<cleaner_upper>&)>& listen_raw
            )
        {
            return [listen_raw] (transaction* trans, const std::shared_ptr<node>& target,
                                const std::function<void(transaction*, const light_ptr&)>& handle,
                                const std::shared_ptr<cleaner_upper>& cleanerUpper) -> std::function<void()> {
                std::shared_ptr<coalesce_state> pState(new coalesce_state);
                return listen_raw(trans, target, [handle, pState, target] (transaction* trans, const light_ptr& ptr) {
                    bool first = !(bool)pState->oValue;
                    pState->oValue = boost::optional<light_ptr>(ptr);
                    if (first)
                        trans.prioritized(rankOf(target), [handle, pState] (const std::shared_ptr<transaction_impl>& impl) {
                            if (pState->oValue) {
                                transaction resurrected(impl);
                                handle(resurrected, pState->oValue.get());
                                pState->oValue = boost::optional<light_ptr>();
                            }
                        });
                }, cleanerUpper);
            };
        }

        std::function<std::function<void()>(transaction*, const std::shared_ptr<node>&,
                         const std::function<void(transaction, const light_ptr&)>&)>
                                             behavior_impl::listen_value_raw() const
        {
            const event_& changes(this->changes);
            const std::function<boost::optional<light_ptr>()>& sample(this->sample);
            return [changes, sample] (transaction* trans, const std::shared_ptr<node>& target,
                               const std::function<void(transaction*, const light_ptr&)>& handle
                           ) -> std::function<void()> {
                const partition<untyped>& part = trans.partition();
                sch::mkevent<void>(part.sequence(), [trans, sample, handle] () {
                    auto oValue = sample();
                    if (oValue)
                        handle(trans, oValue.get());
                })->schedule();
                return changes.listen_raw_(trans, target, handle);
            };
        }

        struct applicative_state {
            applicative_state() {}
            boost::optional<std::function<light_ptr(const light_ptr&)>> oF;
            boost::optional<light_ptr> oA;
            boost::optional<light_ptr> calcB() const {
                if (oF && oA)
                    return boost::optional<light_ptr>(oF.get()(oA.get()));
                else
                    return boost::optional<light_ptr>();
            }
        };

        behavior_ apply(transaction* trans0, const behavior_& bf, const behavior_& ba)
        {
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            boost::optional<light_ptr> ocf = bf.getConstantValue();
            if (ocf) { // function is constant
                auto f = *ocf.get().castPtr<std::function<light_ptr(const light_ptr&)>>(NULL);
                return impl::map(f, ba);  // map optimizes to a constant where ba is constant
            }
            else {
                boost::optional<light_ptr> oca = ba.getConstantValue();
                if (oca) {  // 'a' value is constant but function is not
                    auto a = oca.get();
                    return impl::map([a] (const light_ptr& pf) -> light_ptr {
                        const std::function<light_ptr(const light_ptr&)>& f =
                            *pf.castPtr<std::function<light_ptr(const light_ptr&)>>(NULL);
                        return f(a);
                    }, bf);
                }
                else {
#endif
                    // Non-constant case
                    std::shared_ptr<applicative_state> state(new applicative_state);

                    auto p = impl::unsafe_new_event();
                    auto push = std::get<1>(p);
                    auto target = std::get<2>(p);
                    auto update = [state, push] ( trans) {
                        auto oo = state->calcB();
                        if (oo)
                            push(trans, oo.get());
                    };
                    auto kill1 = bf.listen_value_raw(trans0, target,
                            [state, update] ( trans, const light_ptr& pf) {
                        state->oF = boost::make_optional(*pf.castPtr<std::function<light_ptr(const light_ptr&)>>(NULL));
                        update(trans);
                    });
                    auto kill2 = ba.listen_value_raw(trans0, target,
                            [state, update] ( trans, const light_ptr& pa) {
                        state->oA = boost::make_optional(pa);
                        update(trans);
                    });
                    return behavior_(impl::hold(
                        trans0, boost::optional<light_ptr>(),
                            std::get<0>(p).add_cleanup(kill1)
                                          .add_cleanup(kill2)
                    ));
    #if defined(SODIUM_CONSTANT_OPTIMIZATION)
                }
            }
    #endif
        }

    };  // end namespace impl

    namespace impl {

        /*!
         * Map a function over this event to modify the output value.
         */
        event_ map(const std::function<light_ptr(const light_ptr&)>& f, const event_& ev)
        {
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            if (ev.is_never())
                return event_();
            else
#endif
                return event_(
                    [f,ev] (const frp::transaction& trans0,
                            std::shared_ptr<node> target,
                            const std::function<void(const frp::transaction&, const light_ptr&)>& handle,
                            const std::shared_ptr<frp::cleaner_upper>& cu) {
                        return ev.listen_impl_(trans0, target, [handle, f] (const frp::transaction& trans, const light_ptr& ptr) {
                            handle(trans, f(ptr));
                        }, cu);
                    },
                    ev.get_cleaner_upper()
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
                    , false
#endif
                );
        }

        behavior_ map(const std::function<light_ptr(const light_ptr&)>& f, const behavior_& beh) {
#if defined(SODIUM_CONSTANT_OPTIMIZATION)
            auto ca = beh.getConstantValue();
            if (ca)
                return behavior_(f(ca.get()));
            else
#endif
                return behavior_(
                    NULL,
                    map(f, underlyingevent_(beh)),
                    [f, beh] () -> boost::optional<light_ptr> {
                        boost::optional<light_ptr> oValue = beh.getSample()();
                        return oValue
                            ? boost::optional<light_ptr>(f(oValue.get()))
                            : boost::optional<light_ptr>();
                    }
                );
        }
    };  // end namespace impl
};  // end namespace sodium
