/*
 * Copyright (c) 2018 Jaroslav Jindrak
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef LIBCPP_MEMORY
#define LIBCPP_MEMORY

#include <internal/aux.hpp>
#include <internal/functional/hash.hpp>
#include <internal/memory/addressof.hpp>
#include <internal/memory/allocator_arg.hpp>
#include <internal/memory/type_getters.hpp>
#include <iterator>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace std
{
    /**
     * 20.7.3, pointer traits:
     * Note: The type getters that are used in pointer_traits
     *       and allocator_traits are implemented in
     *       <internal/memory/type_getters.hpp>.
     */

    template<class Ptr>
    struct pointer_traits
    {
        using pointer         = Ptr;
        using element_type    = typename aux::ptr_get_element_type<Ptr>::type;
        using difference_type = typename aux::ptr_get_difference_type<Ptr>::type;

        template<class U>
        using rebind = typename aux::ptr_get_rebind<Ptr, U>::type;

        static pointer pointer_to( // If is_void_t<element_type>, this type is unspecified.
            conditional_t<is_void_v<element_type>, char, element_type&> x
        )
        {
            return Ptr::pointer_to(x);
        }
    };

    template<class T>
    struct pointer_traits<T*>
    {
        using pointer         = T*;
        using element_type    = T;
        using difference_type = ptrdiff_t;

        template<class U>
        using rebind = U*;

        static pointer pointer_to(
            conditional_t<is_void_v<element_type>, char, element_type&> x
        )
        {
            return std::addressof(x);
        }
    };

    /**
     * 20.7.4, pointer safety:
     */

    // TODO: implement

    /**
     * 20.7.5, align:
     */

    // TODO: implement

    /**
     * 20.7.7, uses_allocator:
     */

    namespace aux
    {
        template<class T, class = void>
        struct has_allocator_type: false_type
        { /* DUMMY BODY */ };

        template<class T>
        struct has_allocator_type<T, void_t<typename T::allocator_type>>
            : true_type
        { /* DUMMY BODY */ };
    }

    template<class T, class Alloc, class = void>
    struct uses_allocator
        : aux::value_is<
        bool, aux::has_allocator_type<T>::value && is_convertible_v<
            Alloc, typename T::allocator_type
        >
    >
    { /* DUMMY BODY */ };

    /**
     * 20.7.8, allocator traits:
     */

    template<class Alloc>
    struct allocator_traits
    {
        using allocator_type = Alloc;

        using value_type         = typename Alloc::value_type;
        using pointer            = typename aux::alloc_get_pointer<Alloc>::type;
        using const_pointer      = typename aux::alloc_get_const_pointer<Alloc, pointer>::type;
        using void_pointer       = typename aux::alloc_get_void_pointer<Alloc, pointer>::type;
        using const_void_pointer = typename aux::alloc_get_const_void_pointer<Alloc, pointer>::type;
        using difference_type    = typename aux::alloc_get_difference_type<Alloc, pointer>::type;
        using size_type          = typename aux::alloc_get_size_type<Alloc, difference_type>::type;

        using propagate_on_container_copy_assignment = typename aux::alloc_get_copy_propagate<Alloc>::type;
        using propagate_on_container_move_assignment = typename aux::alloc_get_move_propagate<Alloc>::type;
        using propagate_on_container_swap            = typename aux::alloc_get_swap_propagate<Alloc>::type;
        using is_always_equal                        = typename aux::alloc_get_always_equal<Alloc>::type;

        template<class T>
        using rebind_alloc = typename aux::alloc_get_rebind_alloc<Alloc, T>;

        template<class T>
        using rebind_traits = allocator_traits<rebind_alloc<T>>;

        static pointer allocate(Alloc& alloc, size_type n)
        {
            return alloc.allocate(n);
        }

        static pointer allocate(Alloc& alloc, size_type n, const_void_pointer hint)
        {
            if constexpr (aux::alloc_has_hint_allocate<Alloc, size_type, const_void_pointer>::value)
                return alloc.allocate(n, hint);
            else
                return alloc.allocate(n);
        }

        static void deallocate(Alloc& alloc, pointer ptr, size_type n)
        {
            alloc.deallocate(ptr, n);
        }

        template<class T, class... Args>
        static void construct(Alloc& alloc, T* ptr, Args&&... args)
        {
            if constexpr (aux::alloc_has_construct<Alloc, T, Args...>::value)
                alloc.construct(ptr, forward<Args>(args)...);
            else
                ::new(static_cast<void*>(ptr)) T(forward<Args>(args)...);
        }

        template<class T>
        static void destroy(Alloc& alloc, T* ptr)
        {
            if constexpr (aux::alloc_has_destroy<Alloc, T>::value)
                alloc.destroy(ptr);
            else
                ptr->~T();
        }

        static size_type max_size(const Alloc& alloc) noexcept
        {
            if constexpr (aux::alloc_has_max_size<Alloc>::value)
                return alloc.max_size();
            else
                return numeric_limits<size_type>::max();
        }

        static Alloc select_on_container_copy_construction(const Alloc& alloc)
        {
            if constexpr (aux::alloc_has_select<Alloc>::value)
                return alloc.select_on_container_copy_construction();
            else
                return alloc;
        }
    };

    /**
     * 20.7.9, the default allocator
     */

    template<class T>
    class allocator;

    template<>
    class allocator<void>
    {
        public:
            using pointer       = void*;
            using const_pointer = const void*;
            using value_type    = void;

            template<class U>
            struct rebind
            {
                using other = allocator<U>;
            };
    };

    template<class T>
    T* addressof(T& x) noexcept;

    template<class T>
    class allocator
    {
        public:
            using size_type       = size_t;
            using difference_type = ptrdiff_t;
            using pointer         = T*;
            using const_pointer   = const T*;
            using reference       = T&;
            using const_reference = const T&;
            using value_type      = T;

            template<class U>
            struct rebind
            {
                using other = allocator<U>;
            };

            using propagate_on_container_move_assignment = true_type;
            using is_always_equal                        = true_type;

            allocator() noexcept = default;

            allocator(const allocator&) noexcept = default;

            template<class U>
            allocator(const allocator<U>&) noexcept
            { /* DUMMY BODY */ }

            ~allocator() = default;

            pointer address(reference x) const noexcept
            {
                return addressof(x);
            }

            const_pointer address(const_reference x) const noexcept
            {
                return addressof(x);
            }

            pointer allocate(size_type n, allocator<void>::const_pointer = 0)
            {
                return static_cast<pointer>(::operator new(n * sizeof(value_type)));
            }

            void deallocate(pointer ptr, size_type n)
            {
                ::operator delete(ptr, n);
            }

            size_type max_size() const noexcept
            {
                return numeric_limits<size_type>::max();
            }

            template<class U, class... Args>
            void construct(U* ptr, Args&&... args)
            {
                ::new((void*)ptr) U(forward<Args>(args)...);
            }

            template<class U>
            void destroy(U* ptr)
            {
                ptr->~U();
            }
    };

    template<class T1, class T2>
    bool operator==(const allocator<T1>&, const allocator<T2>&) noexcept
    {
        return true;
    }

    template<class T1, class T2>
    bool operator!=(const allocator<T1>&, const allocator<T2>&) noexcept
    {
        return false;
    }

    /**
     * 20.7.10, raw storage iterator:
     */

    template<class OutputIterator, class T>
    class raw_storage_iterator: public iterator<output_iterator_tag, void, void, void, void>
    {
        public:
            explicit raw_storage_iterator(OutputIterator it)
                : it_{it}
            { /* DUMMY BODY */ }

            raw_storage_iterator& operator*()
            {
                return *this;
            }

            raw_storage_iterator& operator=(const T& element)
            {
                new(it_) T{element};

                return *this;
            }

            raw_storage_iterator& operator++()
            {
                ++it_;

                return *this;
            }

            raw_storage_iterator operator++(int)
            {
                return raw_storage_iterator{it_++};
            }

        private:
            OutputIterator it_;
    };

    /**
     * 20.7.11, temporary buffers:
     */

    template<class T>
    pair<T*, ptrdiff_t> get_temporary_buffer(ptrdiff_t n) noexcept
    {
        T* res{};

        while (n > 0)
        {
            res = (T*)malloc(n * sizeof(T));

            if (res)
                return make_pair(res, n);

            --n;
        }

        return make_pair(nullptr, ptrdiff_t{});
    }

    template<class T>
    void return_temporary_buffer(T* ptr)
    {
        free(ptr);
    }

    /**
     * 20.7.12, specialized algorithms:
     */

    template<class Iterator>
    struct iterator_traits;

    template<class InputIterator, class ForwardIterator>
    ForwardIterator unitialized_copy(
        InputIterator first, InputIterator last,
        ForwardIterator result
    )
    {
        for (; first != last; ++first, ++result)
            ::new (static_cast<void*>(&*result)) typename iterator_traits<ForwardIterator>::value_type(*first);

        return result;
    }

    template<class InputIterator, class Size, class ForwardIterator>
    ForwardIterator unitialized_copy_n(
        InputIterator first, Size n, ForwardIterator result
    )
    {
        for (; n > 0; ++first, --n, ++result)
            ::new (static_cast<void*>(&*result)) typename iterator_traits<ForwardIterator>::value_type(*first);

        return result;
    }

    template<class ForwardIterator, class T>
    void unitialized_fill(
        ForwardIterator first, ForwardIterator last, const T& x
    )
    {
        for (; first != last; ++first)
            ::new (static_cast<void*>(&*first)) typename iterator_traits<ForwardIterator>::value_type(x);
    }

    template<class ForwardIterator, class Size, class T>
    ForwardIterator unitialized_fill_n(
        ForwardIterator first, Size n, const T& x
    )
    {
        for (; n > 0; ++first, --n)
            ::new (static_cast<void*>(&*first)) typename iterator_traits<ForwardIterator>::value_type(x);

        return first;
    }

    /**
     * 20.8, smart pointers:
     */

    template<class T>
    struct default_delete
    {
        default_delete() noexcept = default;

        template<class U, class = enable_if_t<is_convertible_v<U*, T*>, void>>
        default_delete(const default_delete<U>&) noexcept
        { /* DUMMY BODY */ }

        template<class U>
        void operator()(U* ptr)
        {
            delete ptr;
        }
    };

    template<class T>
    struct default_delete<T[]>
    {
        default_delete() noexcept = default;

        template<class U, class = enable_if_t<is_convertible_v<U(*)[], T(*)[]>, void>>
        default_delete(const default_delete<U[]>&) noexcept
        { /* DUMMY BODY */ }

        template<class U, class = enable_if_t<is_convertible_v<U(*)[], T(*)[]>, void>>
        void operator()(U* ptr)
        {
            delete[] ptr;
        }
    };

    template<class T, class D = default_delete<T>>
    class unique_ptr;

    namespace aux
    {
        template<class P, class D, class = void>
        struct get_unique_pointer: type_is<typename P::element_type*>
        { /* DUMMY BODY */ };

        template<class P, class D>
        struct get_unique_pointer<P, D, void_t<typename remove_reference_t<D>::pointer>>
            : type_is<typename remove_reference_t<D>::pointer>
        { /* DUMMY BODY */ };
    }

    template<class T, class D>
    class unique_ptr
    {
        public:
            using element_type = T;
            using deleter_type = D;
            using pointer      = typename aux::get_unique_pointer<unique_ptr<T, D>, D>::type;

            /**
             * 20.8.1.2.1, constructors:
             */

            constexpr unique_ptr() noexcept
                : ptr_{}, deleter_{}
            { /* DUMMY BODY */ }

            explicit unique_ptr(pointer ptr) noexcept
                : ptr_{ptr}, deleter_{}
            { /* DUMMY BODY */ }

            unique_ptr(pointer ptr, /* TODO */ int d) noexcept;

            unique_ptr(pointer ptr, /* TODO */ char d) noexcept;

            unique_ptr(unique_ptr&& other)
                : ptr_{move(other.ptr_)}, deleter_{forward<deleter_type>(other.deleter_)}
            {
                other.ptr_ = nullptr;
            }

            constexpr unique_ptr(nullptr_t)
                : unique_ptr{}
            { /* DUMMY BODY */ }

            template<
                class U, class E,
                class = enable_if_t<
                    is_convertible_v<
                        typename unique_ptr<U, E>::pointer,
                        pointer
                    >, void
                >
            >
            unique_ptr(unique_ptr<U, E>&& other) noexcept
                : ptr_{move(other.ptr_)}, deleter_{forward<D>(other.deleter_)}
            {
                other.ptr_ = nullptr;
            }

            /**
             * 20.8.1.2.2, destructor:
             */

            ~unique_ptr()
            {
                if (ptr_)
                    deleter_(ptr_);
            }

            /**
             * 20.8.1.2.3, assignment:
             */

            unique_ptr& operator=(unique_ptr&& rhs) noexcept
            {
                reset(rhs.release());
                deleter_ = forward<deleter_type>(rhs.get_deleter());

                return *this;
            }

            template<
                class U, class E,
                class = enable_if_t<
                    is_convertible_v<
                        typename unique_ptr<U, E>::pointer,
                        pointer
                    > && !is_array_v<U>, void
                >
            >
            unique_ptr& operator=(unique_ptr<U, E>&& rhs) noexcept
            {
                reset(rhs.release());
                deleter_ = forward<E>(rhs.get_deleter());

                return *this;
            }

            unique_ptr& operator=(nullptr_t) noexcept
            {
                reset();

                return *this;
            }

            /**
             * 20.8.1.2.4, observers:
             */

            add_lvalue_reference_t<element_type> operator*() const
            {
                return *ptr_;
            }

            pointer operator->() const noexcept
            {
                return ptr_;
            }

            pointer get() const noexcept
            {
                return ptr_;
            }

            deleter_type& get_deleter() noexcept
            {
                return deleter_;
            }

            const deleter_type& get_deleter() const noexcept
            {
                return deleter_;
            }

            explicit operator bool() const noexcept
            {
                return ptr_ != nullptr;
            }

            /**
             * 20.8.1.2.5, modifiers:
             */

            pointer release() noexcept
            {
                auto ret = ptr_;
                ptr_ = nullptr;

                return ret;
            }

            void reset(pointer ptr = pointer{}) noexcept
            {
                /**
                 * Note: Order is significant, deleter may delete
                 *       *this.
                 */
                auto old = ptr_;
                ptr_ = ptr;

                if (old)
                    deleter_(old);
            }

            void swap(unique_ptr& other) noexcept
            {
                std::swap(ptr_, other.ptr_);
                std::swap(deleter_, other.deleter_);
            }

            unique_ptr(const unique_ptr&) = delete;
            unique_ptr& operator=(const unique_ptr&) = delete;

        private:
            pointer ptr_;
            deleter_type deleter_;
    };

    namespace aux
    {
        template<class From, class To>
        struct is_convertible_array: is_convertible<From(*)[], To(*)[]>
        { /* DUMMY BODY */ };

        template<class From, class To>
        inline constexpr bool is_convertible_array_v = is_convertible_array<From, To>::value;

        template<class T, class D, class U, class E>
        struct compatible_ptrs: integral_constant<
              bool,
              is_array_v<U> && is_same_v<
                typename unique_ptr<T, D>::pointer,
                typename unique_ptr<T, D>::element_type*
              > && is_same_v<
                typename unique_ptr<U, E>::pointer,
                typename unique_ptr<U, E>::element_type*
              > && is_convertible_array_v<
                typename unique_ptr<T, D>::element_type,
                typename unique_ptr<U, E>::element_type
              > && ((is_reference_v<D> && is_same_v<D, E>) ||
                   (!is_reference_v<D> && is_convertible_v<E, D>))
        >
        { /* DUMMY BODY */ };

        template<class T, class D, class U, class E>
        inline constexpr bool compatible_ptrs_v = compatible_ptrs<T, D, U, E>::value;
    }

    template<class T, class D>
    class unique_ptr<T[], D>
    {
        public:
            using element_type = T;
            using deleter_type = D;
            using pointer      = typename aux::get_unique_pointer<unique_ptr<T[], D>, D>::type;

            /**
             * 20.8.1.3.1, constructors:
             */

            constexpr unique_ptr() noexcept
                : ptr_{}, deleter_{}
            { /* DUMMY BODY */ }

            template<
                class U,
                class = enable_if_t<
                    is_same_v<U, T> || aux::is_convertible_array_v<U, T>, void
                >
            >
            explicit unique_ptr(U ptr) noexcept
                : ptr_{ptr}, deleter_{}
            { /* DUMMY BODY */ }

            template<
                class U, class E,
                class = enable_if_t<aux::compatible_ptrs_v<T, D, U, E>, void>
            >
            unique_ptr(U ptr, /* TODO */ int d) noexcept;

            template<
                class U, class E,
                class = enable_if_t<aux::compatible_ptrs_v<T, D, U, E>, void>
            >
            unique_ptr(U ptr, /* TODO */ char d) noexcept;

            unique_ptr(unique_ptr&& other) noexcept
                : ptr_{move(other.ptr_)}, deleter_{forward<deleter_type>(other.deleter_)}
            {
                other.ptr_ = nullptr;
            }

            template<
                class U, class E,
                class = enable_if_t<
                    is_same_v<U, pointer> ||
                    (is_same_v<pointer, element_type*> && is_pointer_v<U> &&
                     aux::is_convertible_array_v<remove_pointer_t<U>, element_type>),
                    void
                >
            >
            unique_ptr(unique_ptr<U, E>&& other) noexcept
                : ptr_{move(other.ptr_)}, deleter_{forward<D>(other.deleter_)}
            {
                other.ptr_ = nullptr;
            }

            constexpr unique_ptr(nullptr_t) noexcept
                : unique_ptr{}
            { /* DUMMY BODY */ }

            ~unique_ptr()
            {
                if (ptr_)
                    deleter_(ptr_);
            }

            /**
             * 20.8.1.3.2, assignment:
             */

            unique_ptr& operator=(unique_ptr&& rhs) noexcept
            {
                reset(rhs.release());
                deleter_ = forward<deleter_type>(rhs.get_deleter());

                return *this;
            }

            template<
                class U, class E,
                class = enable_if_t<aux::compatible_ptrs_v<T, D, U, E>, void>
            >
            unique_ptr& operator=(unique_ptr<U, E>&& rhs) noexcept
            {
                reset(rhs.release());
                deleter_ = forward<E>(rhs.get_deleter());

                return *this;
            }

            unique_ptr& operator=(nullptr_t) noexcept
            {
                reset();

                return *this;
            }

            /**
             * 20.8.1.3.3, observers:
             */

            element_type& operator[](size_t idx) const
            {
                return ptr_[idx];
            }

            pointer get() const noexcept
            {
                return ptr_;
            }

            deleter_type& get_deleter() noexcept
            {
                return deleter_;
            }

            const deleter_type& get_deleter() const noexcept
            {
                return deleter_;
            }

            explicit operator bool() const noexcept
            {
                return ptr_ != nullptr;
            }

            /**
             * 20.8.1.3.4, modifiers:
             */

            pointer release() noexcept
            {
                auto ret = ptr_;
                ptr_ = nullptr;

                return ret;
            }

            template<
                class U,
                class = enable_if_t<
                    is_same_v<U, pointer> ||
                    (is_same_v<pointer, element_type*> && is_pointer_v<U> &&
                     aux::is_convertible_array_v<remove_pointer_t<U>, element_type>),
                    void
                >
            >
            void reset(U ptr) noexcept
            {
                /**
                 * Note: Order is significant, deleter may delete
                 *       *this.
                 */
                auto old = ptr_;
                ptr_ = ptr;

                if (old)
                    deleter_(old);
            }

            void reset(nullptr_t = nullptr) noexcept
            {
                reset(pointer{});
            }

            void swap(unique_ptr& other) noexcept
            {
                std::swap(ptr_, other.ptr_);
                std::swap(deleter_, other.deleter_);
            }

            unique_ptr(const unique_ptr&) = delete;
            unique_ptr& operator=(const unique_ptr&) = delete;

        private:
            pointer ptr_;
            deleter_type deleter_;
    };

    namespace aux
    {
        template<class T>
        struct is_unbound_array: false_type
        { /* DUMMY BODY */ };

        template<class T>
        struct is_unbound_array<T[]>: true_type
        { /* DUMMY BODY */ };

        template<class T>
        struct is_bound_array: false_type
        { /* DUMMY BODY */ };

        template<class T, size_t N>
        struct is_bound_array<T[N]>: true_type
        { /* DUMMY BODY */ };
    }

    template<
        class T, class... Args,
        class = enable_if_t<!is_array_v<T>, void>
    >
    unique_ptr<T> make_unique(Args&&... args)
    {
        return unique_ptr<T>(new T(forward<Args>(args)...));
    }

    template<
        class T, class = enable_if_t<aux::is_unbound_array<T>::value, void>
    >
    unique_ptr<T> make_unique(size_t n)
    {
        return unique_ptr<T>(new remove_extent_t<T>[n]());
    }

    template<
        class T, class... Args,
        class = enable_if_t<aux::is_bound_array<T>::value, void>
    >
    void make_unique(Args&&...) = delete;

    template<class T, class D>
    void swap(unique_ptr<T, D>& lhs, unique_ptr<T, D>& rhs) noexcept
    {
        lhs.swap(rhs);
    }

    template<class T1, class D1, class T2, class D2>
    bool operator==(const unique_ptr<T1, D1>& lhs,
                    const unique_ptr<T2, D2>& rhs)
    {
        return lhs.get() == rhs.get();
    }

    template<class T1, class D1, class T2, class D2>
    bool operator!=(const unique_ptr<T1, D1>& lhs,
                    const unique_ptr<T2, D2>& rhs)
    {
        return lhs.get() != rhs.get();
    }

    template<class T1, class D1, class T2, class D2>
    bool operator<(const unique_ptr<T1, D1>& lhs,
                   const unique_ptr<T2, D2>& rhs)
    {
        return lhs.get() < rhs.get();
    }

    template<class T1, class D1, class T2, class D2>
    bool operator<=(const unique_ptr<T1, D1>& lhs,
                    const unique_ptr<T2, D2>& rhs)
    {
        return !(rhs < lhs);
    }

    template<class T1, class D1, class T2, class D2>
    bool operator>(const unique_ptr<T1, D1>& lhs,
                   const unique_ptr<T2, D2>& rhs)
    {
        return rhs < lhs;
    }

    template<class T1, class D1, class T2, class D2>
    bool operator>=(const unique_ptr<T1, D1>& lhs,
                    const unique_ptr<T2, D2>& rhs)
    {
        return !(lhs < rhs);
    }

    template<class T, class D>
    bool operator==(const unique_ptr<T, D>& ptr, nullptr_t) noexcept
    {
        return !ptr;
    }

    template<class T, class D>
    bool operator==(nullptr_t, const unique_ptr<T, D>& ptr) noexcept
    {
        return !ptr;
    }

    template<class T, class D>
    bool operator!=(const unique_ptr<T, D>& ptr, nullptr_t) noexcept
    {
        return static_cast<bool>(ptr);
    }

    template<class T, class D>
    bool operator!=(nullptr_t, const unique_ptr<T, D>& ptr) noexcept
    {
        return static_cast<bool>(ptr);
    }

    template<class T, class D>
    bool operator<(const unique_ptr<T, D>& ptr, nullptr_t)
    {
        return ptr.get() < nullptr;
    }

    template<class T, class D>
    bool operator<(nullptr_t, const unique_ptr<T, D>& ptr)
    {
        return nullptr < ptr.get();
    }

    template<class T, class D>
    bool operator<=(const unique_ptr<T, D>& ptr, nullptr_t)
    {
        return !(nullptr < ptr);
    }

    template<class T, class D>
    bool operator<=(nullptr_t, const unique_ptr<T, D>& ptr)
    {
        return !(ptr < nullptr);
    }

    template<class T, class D>
    bool operator>(const unique_ptr<T, D>& ptr, nullptr_t)
    {
        return nullptr < ptr;
    }

    template<class T, class D>
    bool operator>(nullptr_t, const unique_ptr<T, D>& ptr)
    {
        return ptr < nullptr;
    }

    template<class T, class D>
    bool operator>=(const unique_ptr<T, D>& ptr, nullptr_t)
    {
        return !(ptr < nullptr);
    }

    template<class T, class D>
    bool operator>=(nullptr_t, const unique_ptr<T, D>& ptr)
    {
        return !(nullptr < ptr);
    }

    /**
     * 20.8.2.7, smart pointer hash support:
     */

    template<class T, class D>
    struct hash<unique_ptr<T, D>>
    {
        size_t operator()(const unique_ptr<T, D>& ptr) const noexcept
        {
            return hash<typename unique_ptr<T, D>::pointer>{}(ptr.get());
        }

        using argument_type = unique_ptr<T, D>;
        using result_type   = size_t;
    };
}

#endif
